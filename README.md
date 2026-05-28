# BluePawz Receiver 🐾

The base station half of the BluePawz cat tracker. Runs on a
**Heltec Wireless Tracker V2** and acts as:

- a LoRa receiver for telemetry from up to 5 collars,
- a Wi-Fi web server with a live Leaflet.js map and a settings panel,
- a BLE beacon that the collars look for to decide "is the cat home?",
- a downlink command transmitter (mode changes, status requests, renames),
- a tiny on-board ST7735 TFT status display.

This repo is the receiver-side firmware. The collar firmware lives at
[**BluePawzTransmitter**](https://github.com/rees3901/BluePawzTransmitter) —
the two need to be in sync.

For the system-level design (wire format, downlink timing, mode profiles)
see [`ARCHITECTURE.md`](./ARCHITECTURE.md).

---

## What's V3?

V3 is the JSON-only protocol generation, ready for first multi-cat rollout.
The repo was briefly migrated to a binary TLV protocol — that work is parked
on the [`wip/binary-migration`](https://github.com/rees3901/BluePawzReceiver/tree/wip/binary-migration)
branch and not on `main`.

### Headline V3 features

- **Dynamic home location** — set from the web UI, stored in LittleFS,
  changeable any time without reflashing. The receiver computes
  distance/bearing per packet using TinyGPSPlus's haversine. The 20 km
  sanity check refuses obvious typos.
- **Opportunistic downlink commands** — when telemetry arrives from a
  collar, queued commands for that collar fire immediately, landing
  inside its 5 s post-TX RX window (Class-A LoRaWAN style).
- **Remote collar rename** — ✏️ button on each cat card in the C&C panel
  prompts for a new name, sends a `set_name` command targeted by the
  immutable numeric `device_id`. Survives the case where the current name
  is the default `Device-N`.
- **ArduinoOTA** — once the receiver is on Wi-Fi, future firmware pushes
  go over the air. USB cable required only for the very first flash.
- **Onboard TFT status panel** — title, Wi-Fi/IP, packet counter, last
  cat + RSSI, home-set state, BLE on/off. 1 Hz refresh.
- **BLE beacon** advertises name `"Home"` at **-12 dBm** TX power so the
  signal doesn't bleed outside the building, paired with an RSSI threshold
  on the collar side for clean indoor-only home detection.

---

## Hardware

| Component | Detail |
|---|---|
| Board | Heltec Wireless Tracker V2 (HTIT-Tracker V2.3) |
| MCU | ESP32-S3FN8 |
| LoRa radio | Semtech SX1262 (built-in, default SPI bus) |
| GPS | UC6580 GNSS (built-in, UART1 @ 115200) |
| Display | ST7735S, 160×80 colour TFT (built-in) |
| Wi-Fi | ESP32-S3 onboard 2.4 GHz |
| BLE | ESP32-S3 onboard |
| Power | USB-C (mains powered — not a battery device) |

### Pin map (verified against schematic HTIT-Tracker_V2.3)

```
LoRa SX1262 (HSPI):
  NSS  = 8     SCK  = 9     MOSI = 10    MISO = 11
  RST  = 12    BUSY = 13    DIO1 = 14

GPS UC6580 (UART1 @115200):
  ESP32 RX (from GPS TX) = 33
  ESP32 TX (to GPS RX)   = 34

TFT ST7735S (software SPI to avoid bus contention with LoRa):
  MOSI = 42    SCK  = 41    CS = 38
  DC   = 40    RST  = 39    BL = 21

Vext (active LOW — drive LOW to power GPS + TFT)  = 3
LED (white onboard indicator)                      = 18
PRG / user button                                  = 0
```

### LoRa radio settings (must match the collars)

| Setting | Value |
|---|---|
| Frequency | 915.0 MHz (US) / 868.0 MHz (EU) — set in `config.h` |
| Spreading factor | SF8 |
| Bandwidth | 250 kHz |
| Coding rate | 4/5 |
| Preamble | 16 symbols |
| Sync word | 0x12 (private network) |
| CRC | enabled |
| LBT | enabled with random backoff |

---

## Building & flashing

### First time — USB

```bash
cd BluePawzReceiver
pio run -e heltec_wireless_tracker_v2 -t upload
pio run -e heltec_wireless_tracker_v2 -t uploadfs   # upload data/ to LittleFS
```

Hold the **BOOT** button on the Heltec while plugging in if PIO can't see
the chip — that drops it into the download-mode bootloader.

### After that — OTA

```bash
pio run -e heltec_wireless_tracker_v2 -t upload --upload-port cattracker.local
```

Or uncomment the two `upload_protocol = espota` lines in `platformio.ini`
and just run `pio run -t upload`.

If `cattracker.local` doesn't resolve, use the device's IP — the serial
monitor prints it on boot, and the TFT shows it too.

---

## Web UI

Open `http://cattracker.local` or the printed IP in a browser.

### Map

- Live position for each collar via WebSocket (port 81)
- Breadcrumb trail (configurable length in side panel)
- Distance-from-home displayed per cat
- Filter dropdowns to show/hide individual cats

### Settings panel (hamburger top-left)

- **Map Settings**: default zoom, reset-to-home button
- **🏠 Home Location**: lat/lon inputs, "Use Map Centre" helper,
  Save button. Persists to LittleFS; broadcasts to all clients via WS.
- **📶 BLE Beacon**: ON/OFF toggle — turn off to save power if no cats
  use BLE home detection.
- **📍 Marker Settings**: breadcrumb trail length.
- **💾 Data & Logging**: message-count info, export log,
  clear log buttons.
- **ℹ️ System Info**: Wi-Fi status, WebSocket state, page uptime.
- **📡 Command & Control**: one card per known collar. Each card has
  a mode selector + apply button, status-request 🔄 button, and a
  rename ✏️ button.

---

## Versioning

The receiver firmware version lives in
[`include/version.h`](./include/version.h) as a single `#define
BLUEPAWZ_VERSION "x.y.z"`. Semantic versioning:

- **MAJOR** — protocol or hardware-level generation (V3 = JSON wire
  protocol + Heltec V2 hardware). A new major means existing collars
  may no longer talk to the new base station without a reflash.
- **MINOR** — new user-visible features (e.g. battery telemetry,
  geofencing). Backwards-compatible with same-major collars.
- **PATCH** — bug fixes, polish, refactors.

The version is surfaced in three places at runtime:

- the TFT title bar (`BluePaws v3.0.0`)
- the `GET /version` HTTP endpoint (JSON `{"version":"3.0.0"}`)
- the web UI title badge (fetched on page load) and the browser tab title

After bumping `BLUEPAWZ_VERSION`, push via `pio run -t upload` (or
OTA). Refresh the web UI and the badge updates — handy for confirming
that an OTA flash actually landed.

## HTTP API

The receiver exposes these endpoints (mostly used by the web UI, but
useful for scripting too):

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | The web UI (serves `data/index.html`) |
| GET | `/data` | All cached telemetry as JSON |
| GET | `/messages.json` | Full circular log buffer download |
| POST | `/clear-log` | Wipe the LittleFS log file |
| GET | `/node-states` | Current operating-mode state of every known collar |
| POST | `/send-command` | Queue a downlink command (see below) |
| GET | `/home` | Returns `{"lat":...,"lon":...}` |
| POST | `/home?lat=&lon=` | Save new home location (20 km sanity-check) |
| GET | `/version` | Returns `{"version":"3.0.0"}` |

### `/send-command` actions

```http
POST /send-command?device=Podge&action=mode&profile=lost
POST /send-command?device=Podge&action=status
POST /send-command?action=rename&device_id=4&name=Whiskers
```

The `mode` and `status` actions match by collar **name**.
The `rename` action targets by immutable **`device_id`** because the
current name may be the default `Device-N`.

---

## File / directory layout

```
BluePawzReceiver/
├── README.md                  ← you are here
├── ARCHITECTURE.md            ← system-wide design notes
├── platformio.ini             ← build config (Heltec V2 board + libs)
├── partitions_8MB_bigfs.csv   ← partition table with a big LittleFS region
├── include/
│   ├── config.h               ← LoRa radio params, operating modes, shared with TX
│   └── protocol.h             ← binary TLV header (parked — V3 uses JSON)
├── src/
│   └── main.cpp               ← everything: HTTP, WS, LoRa, BLE, TFT, OTA
├── data/                      ← uploaded to LittleFS via `pio run -t uploadfs`
│   ├── index.html
│   ├── Leaflet2_minimal.js
│   └── icons/                 ← per-cat marker icons
├── lib/                       ← project-local libs (rarely used)
└── test/                      ← test scaffolding (mostly empty)
```

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Boot loops + serial silence | Power: check 5 V USB and the orange power LED |
| TFT stays dark | Vext (GPIO 3) not pulled LOW, OR TFT pin map wrong |
| GPS never gets a fix | Vext not enabled, antenna disconnected, or indoors |
| Wi-Fi connect fails | Update `secrets.h` (not committed) with SSID/password |
| Web page loads but no map | `pio run -t uploadfs` skipped — LittleFS is empty |
| Cats never appear on map | Check serial: are JSON packets arriving? Are LoRa params identical between RX and TX (frequency, SF, BW, CR, sync word)? |
| Cat reports "Home" outside | Walk-test the BLE RSSI: lower `HOME_RSSI_THRESHOLD_DBM` in the *transmitter* `config.h` |
| OTA fails | Make sure laptop and base station are on the same Wi-Fi, port 3232 not blocked |

---

## Related repositories

- 🛰️ **Transmitter (collar) firmware**: https://github.com/rees3901/BluePawzTransmitter

The collar repo carries the same `config.h`. Keep the LoRa radio
parameters in lockstep across both repos.

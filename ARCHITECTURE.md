# BluePawz V3 — System Architecture

Same document lives in both
[BluePawzReceiver](https://github.com/rees3901/BluePawzReceiver/blob/main/ARCHITECTURE.md)
and
[BluePawzTransmitter](https://github.com/rees3901/BluePawzTransmitter/blob/main/ARCHITECTURE.md).
When you change one, change the other.

---

## 1. The big picture

```
                    ┌──────────────────────────────────────┐
                    │        Heltec Wireless Tracker V2    │
                    │            (BluePawzReceiver)        │
                    │                                      │
                    │  ┌──────┐  ┌──────┐  ┌─────────────┐ │
                    │  │ LoRa │  │ WiFi │  │ BLE beacon  │ │
                    │  │ SX1262│ │ +UI  │  │ "Home" -12dBm│ │
                    │  └──┬───┘  └──┬───┘  └──────┬──────┘ │
                    └─────┼─────────┼─────────────┼────────┘
                          │         │             │
                LoRa 868MHz│   WiFi/HTTP/WS    BLE
                  SF9/125k │   (port 80, 81)  (short range)
                          │         │             │
                          │         ▼             │
                          │   ┌───────────┐       │
                          │   │  Browser  │       │
                          │   │  (map UI) │       │
                          │   └───────────┘       │
                          │                       │
                  ┌───────┴────────┐              │
                  ▼                ▼              │
            ┌─────────┐      ┌─────────┐          │
            │ Collar 1│ ...  │ Collar 5│ ◀────────┘ scanned by collars
            │ (XIAO   │      │ (XIAO   │           to detect "I'm home"
            │  ESP32S3)│     │  ESP32S3)│
            └─────────┘      └─────────┘
              ▲                 ▲
              │                 │
            GPS               GPS
        (NEO-6M / UC6580)
```

Five wearable collars send GPS positions over LoRa to one mains-powered
base station. The base station serves a Wi-Fi web UI showing all
positions on a map and lets the user push commands back to individual
collars. The base station also runs a low-power BLE beacon named
`"Home"` that the collars use to detect "I'm indoors, no need to TX".

---

## 2. LoRa link parameters (RX and TX must match exactly)

| Setting | Value |
|---|---|
| Frequency | 868.0 MHz (EU/UK ISM band) |
| Spreading factor | SF9 |
| Bandwidth | 125 kHz |
| Coding rate | 4/5 |
| Preamble | 8 symbols |
| Sync word | 0x12 (private network) |
| CRC | enabled |
| Listen-Before-Talk | enabled, with random backoff on collision |

These values mirror `include/config.h` in both firmware projects. Keep
the two copies synchronized; a mismatch in any radio parameter prevents
the link from working. Sync word `0x12` identifies this private network;
LoRaWAN public networks normally use `0x34`.

---

## 3. Wire format (JSON)

Everything on the radio is human-readable JSON. Frequently repeated wire
keys are deliberately compact to reduce airtime while retaining JSON's
debuggability. The receiver expands them immediately after parsing, so
the web UI, logs, and internal state continue to use descriptive names.

The binary work is preserved on `wip/binary-migration` branches in
both repos for future revival if power becomes a constraint.

### 3.1 Telemetry (collar → base)

Three variants depending on the wake outcome.

**Normal GPS fix**:

```jsonc
{
  "type": "tel",
  "src": 430,                 // source_id
  "dst": 1,                   // destination_id (base)
  "seq": 42,                  // telemetry msg_id
  "name": "Podge",
  "st": "roam",               // status
  "md": "normal",             // mode/profile ("dev" for developer)
  "lat": 51.873782,
  "lon": -2.239428,
  "time": 1781188262          // GPS UTC as Unix seconds
}
```

The same envelope is used for `BLEHome` and `invalidGPSLoc`; only `st`
and the optional location fields differ.

| Wire key | Expanded receiver key | Meaning |
|---|---|---|
| `src` | `source_id` | Sender |
| `dst` | `destination_id` | Intended recipient |
| `mid` | `message_id` | Command ID, echoed by replies |
| `seq` | `msg_id` | Collar telemetry sequence |
| `st` | `status` | Telemetry state |
| `md` | `mode` | Operating profile |

For collar-originated packets, `src` is also the immutable device identity;
the receiver derives its internal `device_id` from it. `type:"tel"` is
normalized to `type:"telemetry"`, and numeric Unix `time` is converted back
to `YYYY-MM-DD HH:MM:SS` UTC before logs or WebSocket clients see it.
`name`, `lat`, and `lon` stay unchanged. The receiver also accepts legacy
`type:"telemetry"`, `did`, and string-formatted `time` during migration.

Compact wire values are normalized before downstream processing:

| Wire value | Receiver/internal value |
|---|---|
| `md:"dev"` | `mode:"developer"` |
| `st:"roam"` | `status:"roaming"` |
| `st:"home"` | `status:"BLEHome"` |
| `st:"last"` | `status:"Last known"` retained GPS fix |
| `type:"ping"` | `type:"presence"` wake announcement |
| `type:"CMD"` | `type:"command"` |

### 3.2 Commands (base → collar)

Commands use the same compact envelope and target immutable numeric IDs.
`src=1` is the base station and `dst=999` is broadcast.

**Mode change**:

```jsonc
{ "type":"CMD", "src":1, "dst":430, "mid":42, "md":"lost" }
```

Valid profiles are `powersave`, `normal`, `active`, `lost`, and
`developer`.

**Rename**:

```jsonc
{ "type":"CMD", "src":1, "dst":430, "mid":43, "name":"Whiskers" }
```

**Ping**:

```jsonc
{ "type":"CMD", "src":1, "dst":430, "mid":44, "ping":true }
```

A ping does not use a separate pong packet type. It causes an immediate
ordinary telemetry response with `pong:true` and `mid` echoing the ping
command. A current GPS fix is used when available; otherwise the last
valid fix retained across deep sleep is returned with `st:"last"` and its
original Unix timestamp. Retained replies omit `sats`; the receiver marks
them as last-known, calculates their age, and treats fixes over 60 minutes
old as stale. The map uses a distinct dashed marker, never adds retained
fixes to breadcrumbs, and will not replace a newer stored or displayed fix
with an older one. If the collar has never obtained a fix, `st` is
`invalidGPSLoc`.

The collar's start-of-wake announcement is separately encoded as
`{"type":"ping",...}`. The command ping above is unambiguous because it
uses `type:"CMD"` with a `ping:true` parameter.

### 3.3 ACKs and NACKs

Configuration commands return compact `type:"ack"` or `type:"nack"`
messages and echo `mid`. Ping is confirmed by its solicited telemetry
instead of an additional ACK.

---

## 4. Downlink timing — Class-A LoRaWAN pattern

This is the most subtle bit of V3 and worth understanding deeply.

### The problem

Collars wake on their own schedule (every 1–20 minutes depending on
mode). The receiver doesn't know when. If the receiver pushes a
queued command "blindly" it almost certainly goes out while the target
collar is in deep sleep — packet lost forever.

GPIO21's active-low user button is also a collar deep-sleep wake source.
A single press starts the normal wake cycle, whose first radio action is
the presence announcement. A second press within 500 ms retains the local
developer-mode toggle.

### The fix

Two pieces, mirror images of each other:

**On the collar (transmitter `main.cpp`, `loop()`)**:

```cpp
// After EV_TXDONE, hold TaskLoRa alive for 5 s of RX before sleeping.
// Each EV_LORA_CMD received during the window extends the deadline
// by 3 s, so a burst of commands in one cycle all land.
xEventGroupClearBits(evBits, EV_LORA_CMD);  // ignore pre-TX commands

uint32_t deadline = millis() + POST_TX_LISTEN_MS;
while (millis() < deadline) {
    if (xEventGroupGetBits(evBits) & EV_LORA_CMD) {
        xEventGroupClearBits(evBits, EV_LORA_CMD);
        deadline = millis() + POST_TX_EXTEND_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}
// then deep-sleep
```

**On the receiver (`handleLoRaPacketJSON`)**:

```cpp
// Telemetry just arrived → the collar is in its 5 s RX window NOW.
// Fire any matching command immediately, bypassing the 3 s safety gate.
String reporting = doc["id"].as<String>();
transmitCommandForDevice(reporting);
```

### Timing budget

```
Collar TX ends ────────────────────────────────► t = 0
Collar radio TX → RX                              t ≈ 5 ms
Collar RX window OPEN, 5000 ms budget             t ≈ 5 ms
Receiver decodes last symbol                      t ≈ 10 ms
Receiver runs JSON handler                        t ≈ 30-60 ms
Receiver lora.transmit() begins                   t ≈ 35-65 ms
                                                    ▲
                                          ~75× headroom inside the 5 s window
```

The receiver's processing is dominated by JSON parsing + WebSocket
broadcast + (rarely) a LittleFS flush. Even pessimistic worst case
(~200 ms) leaves >24× headroom.

### What about a busy receiver?

If the receiver was *already* transmitting a different command to a
different collar when the new telemetry arrived, the inbound packet
would be missed (radio in TX state). With 5 collars on 5-minute
cycles the odds of overlap are ~1 in a few thousand cycles. Not worth
mitigating for the V3 rollout. If we ever scale to 20 collars we'd
revisit.

---

## 5. Operating modes

Defined in `config.h` on both sides as an `OperatingMode` struct array.
Switching is via a mode command; the new mode is persisted to NVS on
the collar and survives a reset.

| Mode | TX dBm | Sleep | LED | Purpose |
|---|---|---|---|---|
| `normal` | 17 | 5 min | 5 flashes | Default |
| `powersave` | 10 | 20 min | 5 flashes | Cat is reliably indoors |
| `active` | 17 | 1 min | 5 flashes | Recent activity / actively watching |
| `lost` | 22 | 30 s | continuous beacon | Cat missing |
| `developer` | 14 | 30 s | 3 flashes | Rapid diagnostics and testing |

### Lost-mode auto-revert (the subtle bug we fixed)

The original code stored `g_lostModeStartTime = millis() / 1000` and
checked `millis()/1000 - g_lostModeStartTime >= 7200`. But **`millis()`
resets to 0 on every deep-sleep wake**, so the subtraction underflowed
or didn't accumulate — the 2 h timer never fired correctly.

V3 replaces the timestamp with an accumulator:

```cpp
RTC_DATA_ATTR uint32_t g_lostModeAccumS = 0;

// Before each deep_sleep_start (only when in lost mode):
g_lostModeAccumS += (millis() / 1000) + upcomingSleepS;

// At the top of each wake (in setup):
if (g_lostModeAccumS >= LOST_MODE_MAX_DURATION_S) {
    saveOperatingMode("normal");
    g_lostModeAccumS = 0;
}
```

When the revert fires, the collar silently saves the new mode. **No
special alert packet** — the previous version sent one with no
`status` field, which the receiver's JSON normaliser tagged as
`"Error"` and the cat dropped off the map at the exact moment recovery
should have happened. Now the next routine telemetry packet carries
`mode: "normal"` and the change shows up naturally.

---

## 6. BLE "Home" detection

The receiver advertises a non-connectable BLE beacon named `"Home"` at
**-12 dBm TX power** (intentionally weak, short range only). Each
collar runs an active BLE scan during its initial 10 s wake window and
during GPS acquisition.

The web map also reflects the receiver's network role. While joined to the
configured home Wi-Fi, the `MyDevice` marker uses `icons/Home.avif`. In
roaming/AP mode it uses the portable `icons/Device_Marker.avif` marker.

Roaming networking prioritizes a stable phone connection: the receiver serves
an AP-only `BluePaws-Roaming` hotspot at `192.168.4.1`, with no captive DNS.
Blocking home-SSID scans are deferred while any hotspot client is connected.
BLE collar discovery uses a two-second scan window every ten seconds rather
than a continuous high-duty scan, reducing contention for the ESP32-S3's
shared Wi-Fi/BLE radio.

A scan hit only counts as "home" if **all three** conditions match:

1. The advertised name equals `"Home"` (case-sensitive — pre-V3 the
   receiver advertised `"HOME"` and the collar checked `"Home"`, so
   home detection was silently broken).
2. The beacon includes an RSSI reading (`haveRSSI()` true).
3. RSSI ≥ `HOME_RSSI_THRESHOLD_DBM` (default `-90`).

The combination — beacon at -12 dBm, threshold at -90 dBm — keeps
"home" reliably confined to indoors. Walk-test to tune; the threshold
is visible in the shared configuration and serial diagnostics.

Each profile has its own `home_heartbeat_cycles` value. After that many
consecutive home-detected wakes the collar sends a wire `st:"home"` heartbeat.
This avoids transmitting on every indoor wake.

---

## 7. State persistence map (transmitter)

The collar has three persistence tiers:

| Tier | Survives | Used for |
|---|---|---|
| `RTC_DATA_ATTR` (RTC memory) | deep sleep only — lost on full reset / USB unplug | counters, mode, lost timer, home cycles, GPS warm state, and last valid GPS fix |
| `Preferences` (NVS, in flash) | everything except erase-flash | `g_senderName` (set via `set_name`), backup `msg_id` every 10 packets |
| `LittleFS` (in flash) | as NVS | `/track_log.csv` (3 MB capped, rotated) |

The receiver has two tiers:

| Tier | Survives | Used for |
|---|---|---|
| In-memory maps keyed by numeric device ID | reboot wipes working state | per-collar state, trails, pending commands |
| `LittleFS` | reboots | home location, telemetry snapshot, developer mode, and circular message log |

---

## 8. Identity model

Every collar has:

- **`DEVICE_ID_INT`** — immutable numeric (100–998), derived from the
  ESP32 MAC address and used for command targeting.
- **`g_senderName`** — friendly label (`"Podge"`, `"Macy"`, `"Device-4"`),
  lives in NVS, changes any time via `set_name`. Pure UX, no
  load-bearing role in the protocol.

Commands target the immutable numeric **device_id**. Friendly names are
display labels only, so renaming a collar cannot break command routing.
Receiver state, trails, and pending commands are keyed by device_id.

---

## 9. Where things compile to / live

```
TRANSMITTER REPO            RECEIVER REPO
─────────────────────       ───────────────────────────
include/config.h    ◄─────► include/config.h    (must match)
include/protocol.h    ←     include/protocol.h  (both parked on
   (deleted on V3)              (V3 #if 0)       wip/binary-migration)
src/main.cpp                src/main.cpp
                            data/
                              index.html        (web UI)
                              Leaflet2_minimal.js
                              icons/
```

The `config.h` files are **expected to be byte-identical** at the LoRa
parameters section. If they diverge by even one value (e.g. sync word),
the radio link silently fails. Worth a script-based sync check in a
future cleanup.

---

## 10. The "binary TLV" branch — what & why

A binary TLV protocol was prototyped in `protocol.h` (parked in both
repos behind `#if 0` and on the `wip/binary-migration` branches). It
would have shrunk telemetry packets from ~150 bytes JSON to ~50 bytes
binary, with CRC-16 and TLV-encoded fields.

We chose JSON for V3 because:

- 5 collars × ~12 packets/hour × 150 bytes is trivial airtime
- JSON is debuggable from any serial monitor
- The binary protocol introduced subtle interop bugs (CRC mismatches,
  enum mismatches between receiver and transmitter)
- We'd rather get reliable real-world data first and optimise later

The binary work is preserved verbatim on the WIP branches and can be
revived if/when one of these changes:

- Scaling to 20+ collars puts pressure on duty cycle
- Battery life becomes the dominant constraint and we need to cut TX
  airtime by 60%
- We want to use payload encryption (TLV is friendlier to libsodium
  than JSON-in-the-clear)

---

## 11. Versioning

Semantic versioning, `MAJOR.MINOR.PATCH`.

| Level | Bump when… | Examples |
|---|---|---|
| **MAJOR** | the wire format, hardware target, or anything that breaks compatibility between receiver and any collar in the fleet | V3 itself (JSON protocol + Heltec V2); a future V4 if we move to binary or encrypt the link |
| **MINOR** | adding a new user-visible feature, backwards-compatible | adding battery telemetry, geofence alerts, breadcrumb persistence |
| **PATCH** | bug fixes, polish, refactors with no functional change | the BLE name case fix, the lost-mode accumulator fix |

Versions are owned per-repo:

- **Receiver** version lives in
  [`BluePawzReceiver/include/version.h`](https://github.com/rees3901/BluePawzReceiver/blob/main/include/version.h)
  as `#define BLUEPAWZ_VERSION`. Surfaced on the TFT, on the web UI
  title, and at `GET /version`.
- **Transmitter** version is currently uncoupled from the receiver's and is
  intentionally omitted from routine telemetry to conserve airtime.

The two MAJORs should always agree (a V3 receiver must only talk to V3
collars). MINOR / PATCH can drift between halves — receivers are usually
ahead because we OTA them more often.

### When to bump in practice

- Commits that don't change behaviour (typos, comments, doc) — no bump.
- Commits that change behaviour but fix something — bump PATCH.
- Commits that expose new functionality to the user — bump MINOR.
- Commits that break compatibility with a deployed collar — bump MAJOR
  and write a migration plan.

## 12. Glossary

- **LBT** — Listen Before Talk. The radio scans the channel for an
  in-progress transmission before keying up. Random backoff on collision.
- **NVS** — ESP32 Non-Volatile Storage (key-value KV in flash).
  Accessed via `Preferences`.
- **RTC memory** — fast SRAM that survives deep sleep (but not reset).
  Accessed via `RTC_DATA_ATTR`.
- **Class A** — LoRaWAN device class where the device speaks first and
  opens an RX window after. The pattern V3 uses for downlink.
- **TLV** — Type-Length-Value. The binary encoding scheme used in the
  parked binary protocol.
- **Vext** — External voltage rail on Heltec boards, switches power
  to the GPS + TFT. Active LOW.

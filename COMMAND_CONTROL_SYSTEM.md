# Command & Control System - Implementation Guide

## System Topology

```
PRIMARY PATH (normal operation)

  [Cat Collar] --(SX1262 LoRa)--> [Home Hub] --(Wi-Fi)--> Cloud

DOWNLINK (commands -- LoRa only)

  [Home Hub]   --(SX1262 LoRa)--> [Target Collar]

FALLBACK PATH (collar-side, power-scheme driven, rare)

  [Cat Collar] --(Cat-1/NB-IoT cellular)-----------------> Cloud
  (Hub not involved -- no downlink commands on this path)
```

**Many-to-one:** Multiple collars (Podge, Macy, Gizmo, Simba, Carrie) all report to **one** Home Hub. The Hub tracks each collar independently.

---

## Overview

The Command & Control system enables remote management of cat tracker collar nodes via LoRa downlink from the Home Hub. You can change operating modes, query status, and monitor node health in real-time through the web interface.

Commands travel: **Web UI -> Home Hub -> LoRa downlink -> Collar**
Telemetry travels: **Collar -> LoRa uplink -> Home Hub -> Wi-Fi -> Cloud**

All LoRa communication uses a compact **binary TLV protocol** defined in `protocol.h` (replacing the previous JSON format). The Hub still accepts legacy JSON packets for backward compatibility but all new transmissions are binary.

---

## Operating Modes

All modes are defined identically in both collar and Home Hub firmware via **`config.h`** and mapped to `bp_profile_t` enums in **`protocol.h`**.

| # | Mode | Profile Enum | TX Power | Sleep | Comm Path | Use Case |
|---|------|-------------|----------|-------|-----------|----------|
| 1 | Normal (Default) | `PROFILE_NORMAL (0x01)` | 19 dBm | 5 min | LoRa -> Hub -> Cloud | Standard daily tracking |
| 2 | Powersave | `PROFILE_POWERSAVE (0x02)` | 10 dBm | 20 min | LoRa -> Hub -> Cloud | Extend battery (Hub assumed nearby) |
| 3 | Active | `PROFILE_ACTIVE (0x03)` | 19 dBm | 1 min | LoRa -> Hub -> Cloud | Frequent updates; cellular if Hub unreachable |
| 4 | Lost | `PROFILE_LOST (0x04)` | 22 dBm (max) | 30 sec | LoRa -> Hub -> Cloud; cellular backup | Emergency search (drains battery!) |

**Lost Mode** also enables continuous LED beacon every 2 seconds and auto-reverts to Active after 2 hours.
**Note:** There is no command path over cellular. If a collar is on the cellular fallback path the Hub cannot send it commands until it returns to LoRa range.

---

## Binary Packet Protocol

Commands are sent as binary packets over LoRa at **915 MHz, SF8, BW 250kHz, CR 4/5**.

Every packet has a **36-byte fixed header**, an optional **TLV payload** (0-28 bytes), and a **2-byte CRC-16** (CCITT-FALSE). Min 38 bytes, max 66 bytes.

### Header Layout

```
Byte(s)  Size  Field
----------------------------------------------
0        1     Protocol version (0x01)
1-2      2     Device ID (u16, little-endian)
3-6      4     Message sequence number (u32, LE)
7-10     4     Unix timestamp (u32, LE)
11       1     Status (bp_status_t)
12-13    2     Flags (u16): bits 0-3 = packet type,
                            bit4 = FLAG_HAS_GPS,
                            bit5 = FLAG_BLE_HOME,
                            bit6 = FLAG_GPS_WARM
14-17    4     Latitude x 10^7 (i32, signed LE)
18-21    4     Longitude x 10^7 (i32, signed LE)
22-23    2     Battery (mV, u16 LE)
24-25    2     GPS accuracy (metres, u16 LE)
26-27    2     Fix age (seconds, u16 LE)
28-29    2     Speed (cm/s, u16 LE)
30-31    2     Distance from home (metres, u16 LE)
32-33    2     Bearing (degrees, u16 LE)
34       1     TLV payload length (bytes)
35       1     Reserved
36+      N     TLV payload (0-28 bytes)
36+N     2     CRC-16/CCITT-FALSE
```

### TLV Payload (Type-Length-Value)

Each entry: `[type:u8][length:u8][value:N bytes]`, chained back-to-back.

| ID | Name | Value | Purpose |
|----|------|-------|---------|
| 0x01 | TLV_PROFILE | u8 | Operating mode (bp_profile_t) |
| 0x02 | TLV_TX_POWER | i8 | LoRa TX power in dBm |
| 0x03 | TLV_SLEEP_INTERVAL | u16 LE | Sleep duration in seconds |
| 0x04 | TLV_GPS_WARM | u8 | 0=cold start, 1=warm start |
| 0x05 | TLV_HOME_CYCLES | u8 | Consecutive BLE home detections |
| 0x06 | TLV_LOG_INFO | u16+u16 | Log entries + size in KB (4 bytes) |
| 0x08 | TLV_LOST_MODE_S | u32 LE | Seconds elapsed in lost mode |
| 0x09 | TLV_NEW_MODE | u8 | Mode reverted to after timeout |
| 0x0A | TLV_DURATION_S | u32 LE | Total duration in seconds |
| 0x0B | TLV_CMD_MSG_ID | u32 LE | msg_seq of the command being ACK'd |

### Packet Types (lower 4 bits of flags field)

| Value | Name | Direction | Purpose |
|-------|------|-----------|---------|
| 0x01 | PKT_TELEMETRY | TX -> RX | Position update / BLE home / GPS error |
| 0x02 | PKT_MODE_ACK | TX -> RX | Confirms mode change was applied |
| 0x03 | PKT_STATUS_RESP | TX -> RX | Reply to status query |
| 0x04 | PKT_ALERT | TX -> RX | Lost mode timeout notification |
| 0x05 | PKT_CMD_MODE | RX -> TX | Command: change operating profile |
| 0x06 | PKT_CMD_STATUS | RX -> TX | Command: request status report |

### Status Values (byte 11)

| Value | Name | Meaning |
|-------|------|---------|
| 0x00 | STATUS_UNKNOWN | Not yet determined |
| 0x01 | STATUS_OUT_AND_ABOUT | Outdoors with valid GPS fix |
| 0x02 | STATUS_BLE_HOME | BLE home beacon detected |
| 0x03 | STATUS_INVALID_GPS | GPS timeout / no fix |
| 0x04 | STATUS_OK | General positive (responses) |
| 0x05 | STATUS_LOST_TIMEOUT | Lost mode auto-timed-out |

### Device Registry

Devices are identified by numeric IDs defined in `protocol.h`:

| ID | Name |
|----|------|
| 0x0000 | Home Hub concentrator (DEVICE_ID_HUB) |
| 0x0001 | Macy |
| 0x0002 | Gizmo |
| 0x0003 | Simba |
| 0x0004 | Podge |
| 0x0005 | Carrie |
| 0xFFFF | Broadcast (all collars) |

---

## Example Packets

### Home Hub -> Collar: Mode Change Command (PKT_CMD_MODE, LoRa downlink)

Change Podge to Lost mode (41 bytes total):

```cpp
uint8_t buf[BP_MAX_PACKET_SIZE];
pkt_init(buf, /*device_id=*/0x0004, /*msg_seq=*/42, /*time=*/0,
         STATUS_OK, PKT_CMD_MODE);
pkt_add_tlv_u8(buf, TLV_PROFILE, PROFILE_LOST);
uint8_t len = pkt_finalize(buf);  // Appends CRC-16, returns 41
lora.transmit(buf, len);
```

Compare to the old JSON equivalent (87 bytes, no integrity check):

```json
{"cmd":"mode","profile":"lost","device":"Podge","msg_id":42}
```

### Collar -> Home Hub: Mode ACK (PKT_MODE_ACK, LoRa uplink)

Podge confirms mode change and echoes the command's msg_seq:

```
Header: device_id=0x0004, msg_seq=<node seq>, status=STATUS_OK,
        flags=PKT_MODE_ACK (0x0002)
TLVs:   [0x01][0x01][0x04]              TLV_PROFILE = PROFILE_LOST
        [0x02][0x01][0x16]              TLV_TX_POWER = 22
        [0x03][0x02][0x1E][0x00]        TLV_SLEEP_INTERVAL = 30
        [0x0B][0x04][0x2A][0x00][0x00][0x00]  TLV_CMD_MSG_ID = 42
CRC-16: 2 bytes
Total: 53 bytes
```

### Collar -> Home Hub: Telemetry (PKT_TELEMETRY, LoRa uplink)

Regular GPS position update from Podge (Hub relays to Cloud via Wi-Fi):

```
Header: device_id=0x0004, status=STATUS_OUT_AND_ABOUT,
        flags=PKT_TELEMETRY (0x0001) | FLAG_HAS_GPS (0x0010) = 0x0011,
        lat_e7=518970104, lon_e7=-22457700,
        batt_mV=3700, speed_cms=120, dist_home_m=450
TLVs:   TLV_PROFILE=PROFILE_ACTIVE, TLV_TX_POWER=19,
        TLV_SLEEP_INTERVAL=60
CRC-16: 2 bytes
```

---

## Web UI - Command & Control Panel

### Location

**Side Panel -> Command & Control**

### Features

- **Node List:** Shows all tracked TX nodes
- **Mode Badge:** Color-coded current mode (Blue=Normal, Green=Active, Yellow=Powersave, Red=Lost)
- **Last Seen:** Human-readable timestamp ("2m ago", "1h 23m ago")
- **Lost Mode Countdown:** Shows remaining time before auto-revert
- **Mode Selector:** Dropdown to change operating mode
- **Apply Button:** Send mode change command
- **Status Button:** Query current node status

### Visual Indicators

- **Normal Mode:** Blue left border
- **Active Mode:** Green left border
- **Powersave Mode:** Yellow left border
- **Lost Mode:** Red left border + pink background + countdown timer

### Safety Features

- **Confirmation Dialog:** Warns before activating Lost mode
- **Auto-Revert:** Lost mode automatically switches to Active after 2 hours
- **Pending State:** Buttons disable while command is being sent
- **Real-Time Updates:** WebSocket broadcasts update UI instantly

---

## System Flow

### 1. Sending a Mode Change

```
User selects mode -> Click Apply
         |
UI sends POST /send-command?device=Podge&action=mode&profile=lost
         |
Backend resolves "Podge" -> device_id 0x0004 (via getDeviceIdByName())
         |
Builds binary: pkt_init + TLV_PROFILE=PROFILE_LOST + pkt_finalize
         |
Queues to CommandQueue
         |
processCommandQueue() transmits raw bytes via LoRa (rate-limited: 3s)
         |
TX node receives -> CRC check -> device_id match -> Applies mode
         |
TX node sends PKT_MODE_ACK with TLV_CMD_MSG_ID echoing msg_seq
         |
Base receives -> CRC check -> handleBinaryModeAck()
         |
broadcastNodeStates() -> WebSocket sends update -> UI refreshes
```

### 2. Status Query

```
User clicks Status button
         |
POST /send-command?device=Podge&action=status
         |
Builds binary PKT_CMD_STATUS packet
         |
TX node responds with PKT_STATUS_RESP:
  TLV_PROFILE, TLV_TX_POWER, TLV_SLEEP_INTERVAL,
  TLV_GPS_WARM, TLV_HOME_CYCLES, TLV_LOG_INFO
         |
Base parses TLVs -> Updates node state -> WebSocket broadcast
```

### 3. Lost Mode Auto-Revert

```
Lost mode activated on TX node
         |
After 2 hours, TX node sends PKT_ALERT:
  status=STATUS_LOST_TIMEOUT, TLV_DURATION_S=7200,
  TLV_NEW_MODE=PROFILE_ACTIVE
         |
Base receives PKT_ALERT -> Updates state -> WebSocket broadcast
         |
UI shows alert dialog
```

---

## File Structure

### Home Hub (ESP32 firmware)

- **`include/protocol.h`** - Binary TLV protocol (MUST match collar project!)
- **`include/config.h`** - Operating mode definitions + comm path defines (MUST match collar!)
- **`src/main.cpp`** - Binary packet handlers, command queue, LoRa TX/RX, HTTP endpoints, WebSocket

### Frontend (Web UI)

- **`data/index.html`** - Command & Control UI panel + JavaScript functions
- **`data/Leaflet2_minimal.js`** - WebSocket message handler integration

---

## API Endpoints

### GET /node-states

**Description:** Returns current state of all tracked TX nodes
**Response:**

```json
[
  {
    "device": "Podge",
    "mode": "normal",
    "power": 19,
    "sleep": 300,
    "last_seen": 1234567890,
    "mode_known": true,
    "lost_mode_elapsed_s": 0,
    "lost_mode_remaining_s": 0
  }
]
```

Note: The HTTP/WebSocket API uses JSON for browser communication. Only the LoRa link uses binary packets.

### POST /send-command

**Description:** Queue a binary LoRa downlink command to a collar
**Parameters:**

- `device` - Collar name (e.g., "Podge") — resolved to device_id internally
- `action` - "mode" or "status"
- `profile` - (only for mode) "normal", "active", "powersave", or "lost"

**Example:**

```
POST /send-command?device=Podge&action=mode&profile=lost
POST /send-command?device=Simba&action=status
```

**Response:** `200 OK` or `400 Bad Request`

---

## Configuration

### Rate Limiting

- **COMMAND_TX_INTERVAL:** 3000ms (prevent LoRa flooding)
- Commands are queued and sent one at a time

### Lost Mode Safety

- **LOST_MODE_MAX_DURATION_S:** 7200 (2 hours)
- **LOST_MODE_FALLBACK_MODE:** "active"
- Prevents battery drain from extended high-power operation

### Polling

- **Node State Refresh:** Every 5 seconds (HTTP GET /node-states)
- **WebSocket Updates:** Real-time (instant)

---

## Testing Checklist

- [ ] Backend compiles without errors
- [ ] `protocol.h` is identical in RX and TX projects
- [ ] `config.h` is identical in RX and TX projects
- [ ] Web UI loads Command & Control panel
- [ ] Node list populates with tracked devices
- [ ] Mode badges show correct colors
- [ ] Mode change queues binary PKT_CMD_MODE (check Serial hex dump)
- [ ] Status query sends binary PKT_CMD_STATUS
- [ ] PKT_MODE_ACK received, TLV_CMD_MSG_ID matches sent msg_seq
- [ ] PKT_STATUS_RESP received, TLVs extracted correctly
- [ ] PKT_ALERT received on lost mode timeout
- [ ] CRC validation rejects corrupt/noise packets (inject bad byte, verify drop)
- [ ] Legacy JSON packets still handled (backward compatibility)
- [ ] Lost mode shows confirmation dialog
- [ ] Lost mode shows countdown timer
- [ ] Lost mode auto-reverts after 2 hours
- [ ] WebSocket updates UI in real-time

---

## Next Steps

1. **Flash Hub firmware:** Upload updated `main.cpp` to Home Hub ESP32
2. **Upload web files:** Use PlatformIO "Upload Filesystem Image" to deploy updated `index.html`
3. **Update collar firmware:** Copy `protocol.h` and `config.h` to collar project, implement binary packet building and cellular fallback
4. **Test end-to-end:** Send mode change, verify PKT_MODE_ACK, check UI updates
5. **Monitor logs:** Use Serial monitor — look for hex dumps and `[ACK]` / `[LORA]` lines

---

## Notes

- **LoRa Parameters NEVER Change Remotely:** Frequency, SF, BW, CR are fixed in `config.h` — changing them requires physical reflashing
- **protocol.h Must Match:** Collar and Hub must use the identical header to ensure byte-for-byte compatibility
- **CRC Protects Every Packet:** 2-byte CRC-16/CCITT-FALSE; corrupt packets are silently dropped
- **Binary First, JSON Fallback:** Hub detects packet format by checking byte 0: `0x01` = binary, `0x7B` (`{`) = legacy JSON
- **No Downlink Over Cellular:** Commands can only be sent when the collar is within LoRa range of the Hub
- **Lost Mode is Emergency Only:** 22 dBm + 30s sleep + possible cellular fallback drains battery very quickly
- **Cellular-Aware:** Hub "Last Seen" ages out when collar is on cellular path; relay resumes automatically on LoRa return

---

## Troubleshooting

**UI doesn't show nodes:**

- Check `/node-states` endpoint in browser (should return JSON array)
- Verify nodes have sent at least one telemetry packet (triggers state tracking)
- Check browser console for JavaScript errors

**Commands not sending:**

- Check Serial monitor for `[LoRa] Transmitting binary command` messages and hex dump
- Verify LoRa radio is initialized (`radio.begin()`)
- Check command queue processing in `loop()`

**No ACK received:**

- Ensure collar has matching `protocol.h` (check version byte and struct sizes)
- Look for `[LORA] Binary CRC validation failed` in Serial — means collar is sending wrong format
- Check LoRa signal strength (may be out of range)
- If collar is on cellular fallback path it cannot receive LoRa downlinks

**Unknown packet type in Serial:**

- Check byte 0: if `0x7B` = `{`, TX is still sending JSON (legacy firmware)
- If byte 0 is `0x01` but packet type unknown, check `flags` field bits 0-3

**Lost mode doesn't auto-revert:**

- PKT_ALERT is sent by the TX node, not the base station — check TX firmware
- Verify `LOST_MODE_MAX_DURATION_S` matches in both `config.h` files
- Check Serial monitor for `STATUS_LOST_TIMEOUT` in received packets

---

**Built for BluePawz Cat Tracker**

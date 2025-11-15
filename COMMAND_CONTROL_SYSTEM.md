# Command & Control System - Implementation Guide

## 🎯 Overview

The Command & Control system enables remote management of cat tracker TX nodes via LoRa from the base station. You can change operating modes, query status, and monitor node health in real-time through the web interface.

---

## 🔧 Operating Modes

All modes are defined identically in both RX and TX projects via **`config.h`**.

### 1️⃣ Normal Mode (Default)

- **TX Power:** 19 dBm
- **Sleep Interval:** 5 minutes
- **LED Flashes:** 5
- **Use Case:** Standard daily tracking

### 2️⃣ Powersave Mode

- **TX Power:** 10 dBm (reduced)
- **Sleep Interval:** 20 minutes
- **LED Flashes:** 5
- **Use Case:** Extend battery when cat is home

### 3️⃣ Active Mode

- **TX Power:** 19 dBm
- **Sleep Interval:** 1 minute
- **LED Flashes:** 5
- **Use Case:** Frequent updates when searching for cat

### 4️⃣ Lost Mode ⚠️

- **TX Power:** 22 dBm (maximum)
- **Sleep Interval:** 30 seconds
- **LED Flashes:** 10 + continuous beacon every 2 seconds
- **Auto-Timeout:** Reverts to Active mode after 2 hours
- **Use Case:** Emergency search mode (drains battery quickly!)

---

## 📡 Command Protocol

Commands are sent as JSON over LoRa at **915 MHz, SF8, BW 250kHz, CR 4/5**.

### Base Station → TX Node

**Mode Change:**

```json
{
  "cmd": "mode",
  "profile": "lost"
}
```

**Status Query:**

```json
{
  "cmd": "get_status"
}
```

### TX Node → Base Station

**ACK Response:**

```json
{
  "ack": "mode",
  "profile": "lost",
  "power": 22,
  "sleep": 30,
  "device": "Podge"
}
```

**Status Response:**

```json
{
  "status": "ok",
  "mode": "normal",
  "battery": 3.7,
  "gps": "locked",
  "uptime": 3600,
  "device": "Podge"
}
```

**Alert (Lost Mode Timeout):**

```json
{
  "alert": "lost_mode_timeout",
  "device": "Podge",
  "old_mode": "lost",
  "new_mode": "active",
  "duration_s": 7200
}
```

---

## 🖥️ Web UI - Command & Control Panel

### Location

**Side Panel → 📡 Command & Control**

### Features

- **Node List:** Shows all tracked TX nodes
- **Mode Badge:** Color-coded current mode (Blue=Normal, Green=Active, Yellow=Powersave, Red=Lost)
- **Last Seen:** Human-readable timestamp ("2m ago", "1h 23m ago")
- **Lost Mode Countdown:** Shows remaining time before auto-revert
- **Mode Selector:** Dropdown to change operating mode
- **Apply Button (✓):** Send mode change command
- **Status Button (🔄):** Query current node status

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

## 🔄 System Flow

### 1. Sending a Mode Change

```
User selects mode → Click Apply (✓)
         ↓
UI sends POST /send-command?device=Podge&action=mode&profile=lost
         ↓
Backend queues command → CommandQueue
         ↓
processCommandQueue() sends via LoRa (rate-limited: 3 sec intervals)
         ↓
TX node receives → Changes mode → Sends ACK
         ↓
Base receives ACK → updateNodeState() parses it
         ↓
broadcastNodeStates() → WebSocket sends update
         ↓
UI updates in real-time → Button re-enables
```

### 2. Status Query

```
User clicks Status (🔄)
         ↓
POST /send-command?device=Podge&action=status
         ↓
Queue {"cmd":"get_status"} → LoRa TX
         ↓
TX node responds with battery, GPS, uptime, etc.
         ↓
Base parses response → Updates node state
         ↓
WebSocket broadcasts → UI shows updated info
```

### 3. Lost Mode Auto-Revert

```
Lost mode activated → lostModeStartTime set
         ↓
Every loop(), checkLostModeTimeout() runs
         ↓
If 2 hours elapsed:
  → Send mode change to "active"
  → Send alert via WebSocket
  → Show alert dialog in UI
```

---

## 📂 File Structure

### Backend (ESP32 Receiver)

- **`include/config.h`** - Shared mode definitions (MUST match TX project!)
- **`src/main.cpp`** - Command queue, LoRa TX, HTTP endpoints, WebSocket broadcasting

### Frontend (Web UI)

- **`data/index.html`** - Command & Control UI panel + JavaScript functions
- **`data/Leaflet2_minimal.js`** - WebSocket message handler integration

---

## 🛠️ API Endpoints

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

### POST /send-command

**Description:** Queue a command to be sent to a TX node  
**Parameters:**

- `device` - TX node ID (e.g., "Podge")
- `action` - "mode" or "status"
- `profile` - (only for mode) "normal", "active", "powersave", or "lost"

**Example:**

```
POST /send-command?device=Podge&action=mode&profile=lost
POST /send-command?device=Smudge&action=status
```

**Response:** `200 OK` or `400 Bad Request`

---

## ⚙️ Configuration

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

## 🧪 Testing Checklist

- [ ] Backend compiles without errors
- [ ] config.h is identical in RX and TX projects
- [ ] Web UI loads Command & Control panel
- [ ] Node list populates with tracked devices
- [ ] Mode badges show correct colors
- [ ] Mode selector updates current mode
- [ ] Apply button sends command (check Serial monitor)
- [ ] Status button queries node (check Serial monitor)
- [ ] Lost mode shows confirmation dialog
- [ ] Lost mode shows countdown timer
- [ ] WebSocket updates UI in real-time
- [ ] Lost mode auto-reverts after 2 hours
- [ ] Alert dialog appears on timeout

---

## 🚀 Next Steps

1. **Flash RX firmware:** Upload updated `main.cpp` to ESP32 base station
2. **Upload web files:** Use PlatformIO's "Upload Filesystem Image" to deploy updated `index.html`
3. **Update TX firmware:** Ensure TX nodes have matching `config.h` and command parsing logic
4. **Test end-to-end:** Send mode change, verify ACK, check real-time UI updates
5. **Monitor logs:** Use Serial monitor to watch LoRa TX/RX and command queue processing

---

## 📝 Notes

- **LoRa Parameters NEVER Change Remotely:** Frequency, spreading factor, bandwidth, and coding rate are fixed in `config.h` and cannot be changed via commands (prevents loss of communication)
- **Mode Definitions Must Match:** Both RX and TX projects use the same `config.h` to ensure consistent behavior
- **Command Queue Prevents Flooding:** 3-second rate limiting ensures LoRa transmission doesn't overload the radio
- **WebSocket Real-Time Updates:** No need to refresh page - UI updates automatically when nodes respond
- **Lost Mode is Emergency Only:** High power + frequent transmissions drain battery very quickly!

---

## 🐛 Troubleshooting

**UI doesn't show nodes:**

- Check `/node-states` endpoint in browser (should return JSON)
- Verify nodes have sent at least one position update (triggers state tracking)
- Check browser console for JavaScript errors

**Commands not sending:**

- Check Serial monitor for "Command queued" messages
- Verify LoRa radio is initialized (`radio.begin()`)
- Check command queue processing in `loop()`

**No ACK received:**

- Ensure TX node has matching `config.h`
- Verify TX node is parsing commands correctly
- Check LoRa signal strength (may be out of range)

**Lost mode doesn't timeout:**

- Check `checkLostModeTimeout()` is called in `loop()`
- Verify `lostModeStartTime` is set when entering lost mode
- Check Serial monitor for timeout messages

---

**Built with ❤️ for BluePawz Cat Tracker**

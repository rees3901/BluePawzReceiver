# Simplified Command & ACK Protocol

## Flow Overview

```
User clicks "Send"
    ↓
Base sends: {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
    ↓
Node applies change
    ↓
Node sends: {"ack":"ok","device":"Podge","msg_id":42}
    ↓
Base confirms: ✅ Command acknowledged
    ↓
Next GPS update includes full status
```

---

## Why Simplified?

**Old ACK (too much):**

```json
{
  "ack": "mode",
  "profile": "active",
  "power": 19,
  "sleep": 60,
  "device": "Podge",
  "msg_id": 42
}
```

**New ACK (just right):**

```json
{ "ack": "ok", "device": "Podge", "msg_id": 42 }
```

**Benefits:**

- ✅ **Less bandwidth** - Smaller packets save battery
- ✅ **Faster** - Quick acknowledgment, full status comes later
- ✅ **Simpler** - Base already knows what it sent
- ✅ **Works with sleeping nodes** - Node wakes, ACKs, goes back to sleep

---

## TX Node Implementation

### Simple ACK Function

```cpp
void sendAck(uint32_t msgId)
{
  JsonDocument doc;
  doc["ack"] = "ok";
  doc["device"] = DEVICE_ID;  // Who is acknowledging
  doc["msg_id"] = msgId;      // Echo back the message ID

  String output;
  serializeJson(doc, output);

  radio.transmit(output);
  Serial.printf("ACK sent (msg_id=%lu)\n", msgId);
}
```

### Command Handler

```cpp
void handleRemoteCommand(String jsonCommand)
{
  JsonDocument doc;
  deserializeJson(doc, jsonCommand);

  // Check if message is for this device
  const char* targetDevice = doc["device"];
  if (!targetDevice || strcmp(targetDevice, DEVICE_ID) != 0) {
    return; // Not for me, ignore
  }

  // Extract message ID
  uint32_t msgId = doc["msg_id"].as<uint32_t>();

  // Process command
  const char* cmd = doc["cmd"];

  if (strcmp(cmd, "mode") == 0) {
    // Apply new mode
    const char* profile = doc["profile"];
    currentMode = getModeByName(profile);

    // Send simple ACK
    sendAck(msgId);

    // Full status will be sent in next GPS update
  }
  else if (strcmp(cmd, "get_status") == 0) {
    // Only send full status when explicitly requested
    sendStatusResponse(msgId);
  }
}
```

### Include Mode in GPS Updates

```cpp
void sendGPSUpdate()
{
  JsonDocument doc;
  doc["id"] = DEVICE_ID;
  doc["lat"] = gps.location.lat();
  doc["lon"] = gps.location.lng();
  doc["mode"] = currentMode->name;        // ← Add current mode
  doc["power"] = currentMode->lora_power_dbm;  // ← Add TX power
  doc["sleep"] = currentMode->sleep_interval_s; // ← Add sleep interval
  // ... other GPS data ...

  String output;
  serializeJson(doc, output);
  radio.transmit(output);
}
```

---

## Sleeping Node Scenario

### Problem:

Node is asleep when command arrives → Can't receive it immediately

### Solution:

Node listens for commands after each GPS transmission

```cpp
void transmitCycle()
{
  // 1. Wake up
  // 2. Get GPS fix
  // 3. Send position update (includes current mode)
  sendGPSUpdate();

  // 4. Listen for commands for 2 seconds
  uint32_t listenStart = millis();
  while (millis() - listenStart < 2000) {
    if (radio.available()) {
      String received = radio.readString();
      handleRemoteCommand(received);
      break; // Command received and ACKed
    }
    delay(10);
  }

  // 5. Go back to sleep
  esp_deep_sleep_start();
}
```

### Timeline Example:

```
00:00 - Node is asleep (Normal mode, 5min interval)
02:30 - User sends "change to Active"
02:30 - Base queues command (msg_id=42)
02:30 - Base transmits command
02:30 - Node is asleep, doesn't receive ❌

05:00 - Node wakes up
05:05 - Node gets GPS, sends position (still shows "Normal")
05:05 - Node enters 2-second listen window
05:05 - Base retransmits command (msg_id=42) ✅
05:05 - Node receives, applies "Active" mode
05:05 - Node sends ACK (msg_id=42) ✅
05:05 - Node goes back to sleep

06:05 - Node wakes again (Active mode, 1min interval)
06:06 - Node sends GPS update (now shows "Active") ✅
06:06 - Base confirms full mode change
```

---

## Base Station Behavior

### Command Queueing

```cpp
// User clicks send in UI
// → HTTP POST /send-command?device=Podge&action=mode&profile=active
// → Backend calls:

sendModeCommand("Podge", "active");

// Which creates:
{
  "cmd": "mode",
  "profile": "active",
  "device": "Podge",
  "msg_id": 42  // Auto-assigned
}
```

### ACK Handling

```cpp
// Receives: {"ack":"ok","device":"Podge","msg_id":42}

void updateNodeState(const JsonDocument &doc)
{
  if (doc["ack"].is<String>()) {
    uint32_t msgId = doc["msg_id"].as<uint32_t>();
    String deviceId = doc["device"].as<String>();

    Serial.printf("[ACK] %s acknowledged (msg_id=%lu) ✅\n",
                  deviceId.c_str(), msgId);

    // Clear pending indicator in UI
    broadcastNodeStates();

    // Trust that node applied the change
    // Full confirmation comes in next GPS update
  }
}
```

---

## UI Feedback Flow

### 1. User Clicks Send

```javascript
// Button shows: ⏳ (pending)
cardModeText.innerHTML =
  '<span style="color: #ffc107;">⏳ Sending command...</span>';
```

### 2. Command Transmitted

```
Serial: [LoRa] TX to Podge (msg_id=42): {"cmd":"mode",...}
```

### 3. ACK Received

```javascript
// WebSocket receives: {"ack":"ok","device":"Podge","msg_id":42}
// Button shows: ✓ (acknowledged)
cardModeText.innerHTML = '<span style="color: #28a745;">✅ Acknowledged</span>';
```

### 4. Next GPS Update

```javascript
// WebSocket receives: {"id":"Podge","mode":"active","power":19,"sleep":60,...}
// UI updates mode badge to "ACTIVE" (green)
cardModeText.innerHTML = '<span style="color: #28a745;">● ACTIVE</span>';
```

---

## Testing

### Serial Monitor - Base Station:

```
[CMD] Queued for Podge (msg_id=42)
[LoRa] TX to Podge (msg_id=42): {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
[LoRa] ✅ Command transmitted successfully (msg_id=42)
[LoRa] RX: {"ack":"ok","device":"Podge","msg_id":42}
[ACK] Podge acknowledged command (msg_id=42) ✅

... waiting for next GPS update ...

[LoRa] RX: {"id":"Podge","lat":51.873,"lon":-2.239,"mode":"active","power":19,"sleep":60}
[GPS] Podge position update - mode confirmed: active
```

### Serial Monitor - TX Node:

```
Waking up...
GPS locked
Sending position: {"id":"Podge","lat":51.873,"lon":-2.239,"mode":"normal",...}
Listening for commands (2sec)...
RX: {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
Command for me (msg_id=42)
Mode changed to: active
ACK sent (msg_id=42)
Going to sleep (Active mode, 60s)...

... 60 seconds later ...

Waking up...
GPS locked
Sending position: {"id":"Podge","lat":51.873,"lon":-2.239,"mode":"active","power":19,"sleep":60}
```

---

## Key Points

✅ **ACK is minimal** - Just `ok`, `device`, and `msg_id`  
✅ **Full status in GPS** - Mode, power, sleep included in regular updates  
✅ **Works when sleeping** - Node processes on wake  
✅ **Tracks commands** - msg_id prevents confusion  
✅ **Battery friendly** - Short ACK packets

Perfect for low-power, long-sleep cat trackers! 🐱

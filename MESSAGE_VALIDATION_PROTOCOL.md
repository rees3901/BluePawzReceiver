# Message Validation Protocol

## Overview

The command system now includes **message_id tracking** and **device targeting** to ensure reliable communication between the base station and multiple TX nodes.

---

## Why Message Validation?

**Problems Solved:**

1. **Multiple nodes** - With 5 different trackers (Podge, Macy, Gizmo, Simba, etc.), we need to ensure each command goes to the right device
2. **Command confirmation** - Base station needs to know which specific command was acknowledged
3. **Message tracking** - Prevents confusion when multiple commands are in-flight

---

## Protocol Structure

### Base Station → TX Node (Command)

```json
{
  "cmd": "mode",
  "profile": "lost",
  "device": "Podge",
  "msg_id": 42
}
```

**Fields:**

- `cmd` - Command type ("mode" or "get_status")
- `profile` - Operating mode (for mode commands)
- `device` - **Target device ID** (TX node checks this)
- `msg_id` - **Unique message ID** (incrementing counter)

### TX Node → Base Station (ACK)

```json
{
  "ack": "mode",
  "profile": "lost",
  "power": 22,
  "sleep": 30,
  "device": "Podge",
  "msg_id": 42
}
```

**Fields:**

- `ack` - Acknowledgment type
- `profile` - Confirmed mode
- `power` - TX power (dBm)
- `sleep` - Sleep interval (seconds)
- `device` - **Sending device ID**
- `msg_id` - **MUST echo back the same msg_id from command**

### TX Node → Base Station (Status Response)

```json
{
  "status": "ok",
  "mode": "normal",
  "battery": 3.7,
  "gps": "locked",
  "uptime": 3600,
  "device": "Podge",
  "msg_id": 42
}
```

**Fields:**

- `status` - Status indicator
- `mode` - Current operating mode
- `battery` - Battery voltage
- `gps` - GPS lock status
- `uptime` - Seconds since boot
- `device` - **Sending device ID**
- `msg_id` - **MUST echo back the same msg_id from request**

---

## Base Station Implementation

### Message ID Counter

```cpp
uint32_t nextMessageId = 1; // Global counter, increments for each command
```

### Command Queue with Message ID

```cpp
struct LoRaCommand
{
  String targetDevice; // "Podge", "Macy", etc.
  String jsonCommand;  // JSON payload
  uint32_t timestamp;  // When queued
  uint32_t messageId;  // Unique message ID
};
```

### Queueing Commands

```cpp
void queueCommand(const String &deviceId, const String &command)
{
  LoRaCommand cmd;
  cmd.targetDevice = deviceId;
  cmd.jsonCommand = command;
  cmd.timestamp = millis();
  cmd.messageId = nextMessageId++; // Assign and increment

  commandQueue.push_back(cmd);

  Serial.printf("[CMD] Queued for %s (msg_id=%lu)\n",
                deviceId.c_str(), cmd.messageId);
}
```

### Transmitting Commands

```cpp
void processCommandQueue()
{
  // ... rate limiting ...

  LoRaCommand cmd = commandQueue.front();
  commandQueue.erase(commandQueue.begin());

  // Parse JSON and inject msg_id and device
  JsonDocument cmdDoc;
  deserializeJson(cmdDoc, cmd.jsonCommand);

  cmdDoc["msg_id"] = cmd.messageId;      // Add message ID
  cmdDoc["device"] = cmd.targetDevice;   // Add target device

  String finalJson;
  serializeJson(cmdDoc, finalJson);

  Serial.printf("[LoRa] TX to %s (msg_id=%lu): %s\n",
                cmd.targetDevice.c_str(), cmd.messageId, finalJson.c_str());

  lora.transmit(finalJson);
}
```

### Receiving ACKs

```cpp
void updateNodeState(const JsonDocument &doc)
{
  String deviceId = doc["device"].as<String>();

  if (doc["ack"].is<String>())
  {
    uint32_t msgId = 0;
    if (doc["msg_id"].is<uint32_t>())
    {
      msgId = doc["msg_id"].as<uint32_t>();
      Serial.printf("[ACK] %s confirmed (msg_id=%lu)\n",
                    deviceId.c_str(), msgId);
    }
    else
    {
      Serial.printf("[ACK] %s confirmed (WARNING: no msg_id!)\n",
                    deviceId.c_str());
    }

    // Process ACK...
  }
}
```

---

## TX Node Implementation

### Device ID Constant

```cpp
#define DEVICE_ID "Podge" // Change for each cat tracker
```

### Command Parsing with Device Filtering

```cpp
void handleRemoteCommand(String jsonCommand)
{
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonCommand);

  if (error) {
    Serial.println("JSON parse error");
    return;
  }

  // ✅ CHECK IF COMMAND IS FOR THIS DEVICE
  const char* targetDevice = doc["device"];
  if (!targetDevice || strcmp(targetDevice, DEVICE_ID) != 0) {
    Serial.printf("Not for me (target=%s, my_id=%s) - IGNORING\n",
                  targetDevice ? targetDevice : "null", DEVICE_ID);
    return; // Silently ignore commands for other devices
  }

  // ✅ EXTRACT MESSAGE_ID
  uint32_t msgId = 0;
  if (doc["msg_id"].is<uint32_t>()) {
    msgId = doc["msg_id"].as<uint32_t>();
    Serial.printf("Command for me (msg_id=%lu)\n", msgId);
  } else {
    Serial.println("WARNING: Command has no msg_id");
  }

  // Process command
  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "mode") == 0) {
    const char* profile = doc["profile"];
    if (profile) {
      currentMode = getModeByName(profile);
      Serial.printf("Mode changed to: %s\n", profile);

      // ✅ SEND ACK WITH MSG_ID
      sendModeAck(profile, msgId);
    }
  }
  else if (strcmp(cmd, "get_status") == 0) {
    // ✅ SEND STATUS WITH MSG_ID
    sendStatusResponse(msgId);
  }
}
```

### Sending ACK (with msg_id echo)

**Simplified version (recommended):**

```cpp
void sendAck(uint32_t msgId)
{
  JsonDocument doc;
  doc["ack"] = "ok";              // Simple acknowledgment
  doc["device"] = DEVICE_ID;      // ✅ Who is responding
  doc["msg_id"] = msgId;          // ✅ Echo back the message ID

  String output;
  serializeJson(doc, output);

  int state = radio.transmit(output);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("ACK sent (msg_id=%lu)\n", msgId);
  } else {
    Serial.printf("ACK TX failed: %d\n", state);
  }
}
```

**Usage:**

```cpp
if (strcmp(cmd, "mode") == 0) {
  const char* profile = doc["profile"];
  if (profile) {
    currentMode = getModeByName(profile);
    Serial.printf("Mode changed to: %s\n", profile);

    // ✅ SEND SIMPLE ACK
    sendAck(msgId);
  }
}
```

**Note:** The full node status (mode, battery, GPS) will be sent in the next regular GPS position update.### Sending Status (with msg_id echo)

```cpp
void sendStatusResponse(uint32_t msgId)
{
  JsonDocument doc;
  doc["status"] = "ok";
  doc["mode"] = currentMode->name;
  doc["battery"] = readBatteryVoltage();
  doc["gps"] = gpsLocked ? "locked" : "searching";
  doc["uptime"] = millis() / 1000;
  doc["device"] = DEVICE_ID;           // ✅ Who is responding
  doc["msg_id"] = msgId;               // ✅ Echo back the message ID

  String output;
  serializeJson(doc, output);

  int state = radio.transmit(output);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("Status sent (msg_id=%lu)\n", msgId);
  } else {
    Serial.printf("Status TX failed: %d\n", state);
  }
}
```

---

## Message Flow Example

### Scenario: Base station sends mode change to Podge

**1. Base Station Queues Command:**

```
[CMD] Queued for Podge (msg_id=42)
```

**2. Base Station Transmits:**

```
[LoRa] TX to Podge (msg_id=42): {"cmd":"mode","profile":"lost","device":"Podge","msg_id":42}
```

**3. Podge Receives (other cats ignore):**

```
Macy: "Not for me (target=Podge, my_id=Macy) - IGNORING"
Gizmo: "Not for me (target=Podge, my_id=Gizmo) - IGNORING"
Podge: "Command for me (msg_id=42)"
Podge: "Mode changed to: lost"
```

**4. Podge Sends ACK:**

```
Podge: "ACK sent (msg_id=42)"
```

**5. Base Station Receives ACK:**

```
[ACK] Podge confirmed (msg_id=42)
[NODE] Podge confirmed mode: lost (Power: 22dBm, Sleep: 30s)
```

**6. WebSocket Updates UI:**

```
WebSocket: {"type":"node_states","nodes":[{"device":"Podge","mode":"lost",...}]}
```

---

## Benefits

### 1. **Minimal Bandwidth**

- ACK is only 3 fields: `{"ack":"ok","device":"Podge","msg_id":42}`
- Saves battery on TX node (less TX time)
- Fast acknowledgment response

### 2. **Sleeping Node Support**

- If node is asleep, it won't ACK immediately
- Base station can detect missing ACK (timeout after 10 seconds)
- When node wakes, it processes command and sends ACK
- Full status comes in next GPS update

### 3. **Device Isolation**

- Each TX node ignores commands not addressed to it
- Prevents accidental mode changes
- Reduces unnecessary processing

### 2. **Message Tracking**

- Base station knows exactly which command was acknowledged
- Can detect missing ACKs (timeout after 10 seconds)
- Enables retry logic in future

### 3. **Debugging**

- Serial logs show exact msg_id flow
- Easy to trace command → ACK relationship
- Clear indication of communication failures

### 4. **Scalability**

- Works with unlimited number of TX nodes
- Each device ID is unique
- Message IDs prevent collision

---

## Serial Monitor Example

### Base Station:

```
[CMD] Queued for Podge (msg_id=42)
[LoRa] TX to Podge (msg_id=42): {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
[LoRa] ✅ Command transmitted successfully (msg_id=42)
[LoRa] RX: {"ack":"mode","profile":"active","power":19,"sleep":60,"device":"Podge","msg_id":42}
[ACK] Podge confirmed (msg_id=42)
[NODE] Podge confirmed mode: active (Power: 19dBm, Sleep: 60s)
WebSocket: Broadcasting node states
```

### TX Node (Podge):

```
LoRa RX: {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
Command for me (msg_id=42)
Mode changed to: active
ACK sent (msg_id=42)
```

### TX Node (Macy):

```
LoRa RX: {"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
Not for me (target=Podge, my_id=Macy) - IGNORING
```

---

## Testing Checklist

- [ ] Base station assigns unique msg_id to each command
- [ ] Commands include `device` field with target ID
- [ ] TX node checks `device` field before processing
- [ ] TX node echoes back `msg_id` in ACK/status
- [ ] Base station logs ACKs with matching msg_id
- [ ] Other TX nodes ignore commands not addressed to them
- [ ] Serial logs show clear msg_id tracking
- [ ] WebSocket updates after ACK received

---

**Result:** Reliable, trackable, multi-device command & control system!

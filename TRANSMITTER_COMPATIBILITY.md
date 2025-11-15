# Transmitter Node Compatibility Guide

## ✅ Summary

Your **BluePawzTransmitter** nodes need the following updates to be compatible with the Command & Control system:

---

## 📋 Required Changes

### 1. **Copy config.h to Transmitter Project**

**Location:** `BluePawzTransmitter/include/config.h`

**Action:** Copy the **IDENTICAL** `config.h` file from this receiver project to your transmitter project.

**Why:** Both devices must use the same mode definitions, LoRa parameters, and command protocol structure.

**Path:**

```
BluePawzReceiver/include/config.h  →  BluePawzTransmitter/include/config.h
```

### 2. **Add Command Parsing to Transmitter**

Your transmitter needs to:

1. Listen for incoming LoRa messages
2. Parse JSON commands
3. Execute mode changes
4. Send ACK/status responses

---

## 🔧 Transmitter Code Requirements

### Include config.h

```cpp
#include <config.h>
```

### Add Global Variables

```cpp
// Current operating mode
const OperatingMode* currentMode = &MODE_NORMAL;
uint32_t lostModeStartTime = 0; // millis() when lost mode started
```

### Add Command Parsing Function

```cpp
void handleRemoteCommand(String jsonCommand) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonCommand);

  if (error) {
    Serial.println("Failed to parse command JSON");
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  // MODE CHANGE COMMAND
  if (strcmp(cmd, "mode") == 0) {
    const char* profile = doc["profile"];
    if (profile) {
      const OperatingMode* newMode = getModeByName(profile);
      currentMode = newMode;

      // Track lost mode start time
      if (strcmp(profile, "lost") == 0) {
        lostModeStartTime = millis();
      } else {
        lostModeStartTime = 0;
      }

      Serial.printf("Mode changed to: %s\n", profile);

      // Send ACK back to base station
      sendModeAck(profile);
    }
  }

  // STATUS REQUEST COMMAND
  else if (strcmp(cmd, "get_status") == 0) {
    sendStatusResponse();
  }
}
```

### Send ACK Response

```cpp
void sendModeAck(const char* profile) {
  JsonDocument doc;
  doc["ack"] = "mode";
  doc["profile"] = profile;
  doc["power"] = currentMode->lora_power_dbm;
  doc["sleep"] = currentMode->sleep_interval_s;
  doc["device"] = DEVICE_ID; // Your device ID (e.g., "Podge")

  String output;
  serializeJson(doc, output);

  // Send via LoRa
  int state = radio.transmit(output);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("ACK sent successfully");
  }
}
```

### Send Status Response

```cpp
void sendStatusResponse() {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["mode"] = currentMode->name;
  doc["battery"] = readBatteryVoltage(); // Your battery reading function
  doc["gps"] = gpsLocked ? "locked" : "searching";
  doc["uptime"] = millis() / 1000;
  doc["device"] = DEVICE_ID;

  String output;
  serializeJson(doc, output);

  int state = radio.transmit(output);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("Status sent successfully");
  }
}
```

### Check for Lost Mode Timeout

Add this to your main `loop()`:

```cpp
void loop() {
  // ... existing code ...

  // Check for lost mode timeout
  if (lostModeStartTime > 0) {
    uint32_t elapsedMs = millis() - lostModeStartTime;
    uint32_t elapsedSecs = elapsedMs / 1000;

    if (elapsedSecs >= LOST_MODE_MAX_DURATION_S) {
      Serial.println("Lost mode timeout - reverting to Active");

      // Send alert to base station
      sendLostModeTimeoutAlert();

      // Revert to active mode
      currentMode = getModeByName(LOST_MODE_FALLBACK_MODE);
      lostModeStartTime = 0;
    }
  }

  // ... rest of loop ...
}
```

### Send Lost Mode Timeout Alert

```cpp
void sendLostModeTimeoutAlert() {
  JsonDocument doc;
  doc["alert"] = "lost_mode_timeout";
  doc["device"] = DEVICE_ID;
  doc["old_mode"] = "lost";
  doc["new_mode"] = LOST_MODE_FALLBACK_MODE;
  doc["duration_s"] = LOST_MODE_MAX_DURATION_S;

  String output;
  serializeJson(doc, output);
  radio.transmit(output);
}
```

### Listen for Commands During Wake Cycle

In your transmit cycle (after sending position data):

```cpp
void transmitCycle() {
  // ... send GPS position ...

  // Listen for incoming commands for 2 seconds
  Serial.println("Listening for commands...");
  uint32_t listenStart = millis();

  while (millis() - listenStart < 2000) { // 2 second listen window
    int state = radio.receive(rxBuffer, MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      String received = String((char*)rxBuffer);
      Serial.printf("Command received: %s\n", received.c_str());
      handleRemoteCommand(received);
      break; // Command received, exit listen mode
    }

    delay(10);
  }

  // ... continue with sleep cycle ...
}
```

---

## 🔄 Integration Checklist

- [ ] Copy `config.h` to transmitter project (must be IDENTICAL)
- [ ] Add `#include <config.h>` to transmitter main.cpp
- [ ] Add global variables: `currentMode`, `lostModeStartTime`
- [ ] Implement `handleRemoteCommand()` function
- [ ] Implement `sendModeAck()` function
- [ ] Implement `sendStatusResponse()` function
- [ ] Implement `sendLostModeTimeoutAlert()` function
- [ ] Add lost mode timeout check to `loop()`
- [ ] Add command listening window after each transmission
- [ ] Update sleep intervals to use `currentMode->sleep_interval_s`
- [ ] Update TX power to use `currentMode->lora_power_dbm`
- [ ] Update LED flashes to use `currentMode->led_flash_count`
- [ ] Implement LED beacon mode (if `currentMode->led_beacon_mode == true`)

---

## ⚙️ Using Current Mode in Transmitter

### Set TX Power

```cpp
radio.setOutputPower(currentMode->lora_power_dbm);
```

### Sleep Interval

```cpp
esp_sleep_enable_timer_wakeup(currentMode->sleep_interval_s * 1000000ULL);
```

### LED Flashes

```cpp
for (uint8_t i = 0; i < currentMode->led_flash_count; i++) {
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(100);
}
```

### LED Beacon (Lost Mode)

```cpp
if (currentMode->led_beacon_mode) {
  // Flash LED continuously while awake
  uint32_t lastBeacon = 0;
  while (!readyToSleep) {
    if (millis() - lastBeacon >= currentMode->led_beacon_interval_ms) {
      digitalWrite(LED_PIN, HIGH);
      delay(50);
      digitalWrite(LED_PIN, LOW);
      lastBeacon = millis();
    }
  }
}
```

---

## 🧪 Testing Sequence

1. **Flash updated transmitter code** with config.h and command handling
2. **Power on transmitter** - should start in NORMAL mode
3. **Send position data** - receiver should track the device
4. **Open Command & Control panel** in web UI
5. **Change mode to ACTIVE** - transmitter should ACK within 2 transmit cycles
6. **Check mode badge** - should update to green "ACTIVE"
7. **Query status** - should receive battery, GPS, uptime
8. **Test LOST mode** - should see confirmation dialog, countdown timer
9. **Wait 2 hours** (or reduce timeout for testing) - should auto-revert to ACTIVE

---

## 🐛 Debugging

### Transmitter Serial Monitor Should Show:

```
Mode changed to: active
ACK sent successfully
Listening for commands...
Command received: {"cmd":"get_status"}
Status sent successfully
```

### Receiver Serial Monitor Should Show:

```
[CMD] Mode change queued: Podge -> active
Command queued: Podge
LoRa TX successful
LoRa RX: {"ack":"mode","profile":"active","power":19,"sleep":60,"device":"Podge"}
[State] Podge mode updated: active (19dBm, 60s sleep)
WebSocket broadcast: node_states
```

### Common Issues:

**No ACK received:**

- Check transmitter is listening for commands after TX
- Verify LoRa parameters match (915MHz, SF8, BW250kHz)
- Check signal strength (may be out of range)

**Mode doesn't change:**

- Verify `handleRemoteCommand()` is being called
- Check JSON parsing (print received string)
- Ensure `currentMode` pointer is updated

**Lost mode doesn't timeout:**

- Check `lostModeStartTime` is set when entering lost mode
- Verify timeout check runs in `loop()`
- Ensure `LOST_MODE_MAX_DURATION_S` is defined in config.h

---

## 📝 Example Minimal Transmitter Integration

```cpp
#include <config.h>
#include <ArduinoJson.h>

#define DEVICE_ID "Podge"

const OperatingMode* currentMode = &MODE_NORMAL;
uint32_t lostModeStartTime = 0;

void setup() {
  // ... existing setup ...

  // Initialize with normal mode
  radio.setOutputPower(currentMode->lora_power_dbm);
}

void loop() {
  // Check lost mode timeout
  if (lostModeStartTime > 0 &&
      (millis() - lostModeStartTime) / 1000 >= LOST_MODE_MAX_DURATION_S) {
    currentMode = &MODE_ACTIVE;
    lostModeStartTime = 0;
    sendLostModeTimeoutAlert();
  }

  // Wake, get GPS, transmit position
  transmitPositionData();

  // Listen for commands
  listenForCommands();

  // Sleep using current mode interval
  esp_sleep_enable_timer_wakeup(currentMode->sleep_interval_s * 1000000ULL);
  esp_deep_sleep_start();
}
```

---

## ✅ Verification

Once implemented, you should be able to:

- ✅ See all transmitter nodes in Command & Control panel
- ✅ Change operating modes remotely
- ✅ See real-time mode changes reflected in UI
- ✅ Query battery status and GPS lock state
- ✅ See countdown timer when in lost mode
- ✅ Automatic revert to active mode after 2 hours in lost mode
- ✅ Control modes from both side panel and marker cards

---

**Questions or issues? Check the receiver's `main.cpp` for reference implementation of command handling and state tracking.**

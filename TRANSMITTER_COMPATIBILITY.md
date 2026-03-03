# Collar (Transmitter Node) Compatibility Guide

## Summary

Your **BluePawzTransmitter** collar nodes need the following updates to be compatible with the Command & Control system. The key change from previous versions is that all LoRa communication now uses the **binary TLV protocol** defined in `protocol.h` instead of JSON.

### Communication Architecture

```
PRIMARY PATH (normal operation — all modes except cellular fallback)

  [Collar] ---(SX1262 LoRa uplink)---> [Home Hub] ---(Wi-Fi)---> Cloud
  [Home Hub] ---(SX1262 LoRa downlink)---> [Collar]  (commands only)

FALLBACK PATH (power-scheme driven, rare)

  [Collar] ---(Cat-1/NB-IoT cellular)---> Cloud  (bypasses Home Hub entirely)
```

> **Important:** Commands from the Home Hub can only reach the collar via LoRa downlink. When the collar is on the cellular path, the Hub has no command channel to it. Mode changes will apply on the next LoRa-path transmit cycle.

---

## Required Changes

### 1. Copy protocol.h and config.h to Transmitter Project

**Locations:**

```
BluePawzReceiver/include/protocol.h  ->  BluePawzTransmitter/include/protocol.h
BluePawzReceiver/include/config.h    ->  BluePawzTransmitter/include/config.h
```

Both files must be **identical** on TX and RX. `protocol.h` defines the binary packet format; `config.h` defines the operating mode parameters.

### 2. Set Your Device ID

In your transmitter's `main.cpp` (or a device-specific header):

```cpp
#define MY_DEVICE_ID 0x0004  // Podge
// Other IDs: Macy=0x0001, Gizmo=0x0002, Simba=0x0003, Carrie=0x0005
```

This must match the `DEVICE_REGISTRY` in `protocol.h`.

### 3. Add Binary Packet Support

Your collar firmware needs to:

1. Build binary telemetry packets (replacing JSON GPS updates)
2. Include `TLV_COMM_PATH` in every packet (LoRa or cellular)
3. Set `FLAG_CELLULAR_UPLINK` when switching to the cellular path
4. Listen for incoming binary commands after each LoRa transmission
5. Validate CRC and device ID before processing
6. Apply mode changes and send binary ACKs
7. Send binary PKT_ALERT on lost mode timeout

---

## Transmitter Code Requirements

### Includes and Global Variables

```cpp
#include <protocol.h>
#include <config.h>

#define MY_DEVICE_ID 0x0004  // Change for each device

const OperatingMode *currentMode = &MODE_NORMAL;
bp_profile_t currentProfile = PROFILE_NORMAL;
uint32_t lostModeStartTime = 0;
uint32_t mySeqCounter = 1;    // Increments with every packet sent
bool gpsWarm = false;
uint8_t bleHomeCycles = 0;
```

### Sending Binary Telemetry (LoRa Path)

Replace your existing JSON GPS update with:

```cpp
void sendLoRaTelemetry(bool hasGps, float lat, float lon, uint16_t accM,
                       uint16_t speedCms, uint16_t distHomeM, uint16_t bearingDeg,
                       uint16_t battMv, uint16_t fixAgeS, bp_status_t status,
                       bool switchingToCellular)
{
  uint8_t buf[BP_MAX_PACKET_SIZE];

  uint16_t flags = PKT_TELEMETRY;
  if (hasGps) flags |= FLAG_HAS_GPS;
  if (bleHomeCycles > 0) flags |= FLAG_BLE_HOME;
  if (gpsWarm) flags |= FLAG_GPS_WARM;
  if (switchingToCellular) flags |= FLAG_CELLULAR_UPLINK; // Last LoRa before cellular

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(), status, flags);

  // Fill header GPS/battery fields
  if (hasGps) {
    int32_t lat_e7 = (int32_t)(lat * 1e7f);
    int32_t lon_e7 = (int32_t)(lon * 1e7f);
    pkt_set_gps(buf, lat_e7, lon_e7, distHomeM, bearingDeg);
    memcpy(&buf[28], &speedCms, 2);
  }
  memcpy(&buf[22], &battMv, 2);
  memcpy(&buf[24], &accM, 2);
  memcpy(&buf[26], &fixAgeS, 2);

  // TLV: current operating mode info
  pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
  pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentMode->lora_power_dbm);
  pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentMode->sleep_interval_s);
  pkt_add_tlv_u8(buf,  TLV_COMM_PATH,      COMM_PATH_LORA); // Reporting via LoRa
  if (gpsWarm) pkt_add_tlv_u8(buf, TLV_GPS_WARM, 1);
  if (bleHomeCycles > 0) pkt_add_tlv_u8(buf, TLV_HOME_CYCLES, bleHomeCycles);

  uint8_t len = pkt_finalize(buf);
  radio.setOutputPower(currentMode->lora_power_dbm);
  radio.transmit(buf, len);
  Serial.printf("LoRa telemetry sent (%d bytes) -> Home Hub -> Cloud\n", len);
}
```

### Cellular Fallback Path

When the power scheme decides to use the cellular modem directly (e.g., Lost mode with no Hub ACK, or configured profile):

```cpp
// Comm path selection — called each wake cycle
uint8_t selectCommPath()
{
  // Lost mode: switch to cellular if Hub hasn't ACKed recent LoRa transmissions
  if (currentProfile == PROFILE_LOST && consecutiveLoRaMisses >= LORA_MISS_THRESHOLD) {
    return COMM_PATH_CELLULAR;
  }
  // All other modes: LoRa to Home Hub is primary
  return COMM_PATH_LORA;
}

// Send last LoRa packet with FLAG_CELLULAR_UPLINK before switching over
void notifyHubSwitchingToCellular()
{
  sendLoRaTelemetry(/* ... */, /* switchingToCellular= */ true);
  // Hub will know it won't see LoRa packets for a while
}

// Cellular telemetry — implementation is modem/platform specific
// The Cat-1/NB-IoT modem transmits directly to the cloud endpoint
void sendCellularTelemetry(/* GPS params */)
{
  // TODO: implement via Cat-1/NB-IoT AT commands or modem library
  // Packet goes directly to cloud — Home Hub is NOT involved
  Serial.println("Cellular telemetry sent -> Cloud (bypassing Home Hub)");
}
```

### Apply an Operating Profile

```cpp
void applyProfile(bp_profile_t profile)
{
  currentProfile = profile;

  switch (profile) {
    case PROFILE_NORMAL:    currentMode = &MODE_NORMAL;    break;
    case PROFILE_POWERSAVE: currentMode = &MODE_POWERSAVE; break;
    case PROFILE_ACTIVE:    currentMode = &MODE_ACTIVE;    break;
    case PROFILE_LOST:
      currentMode = &MODE_LOST;
      lostModeStartTime = millis();
      break;
    default:
      currentMode = &MODE_NORMAL;
      currentProfile = PROFILE_NORMAL;
      break;
  }

  if (profile != PROFILE_LOST) {
    lostModeStartTime = 0;
  }

  Serial.printf("Profile applied: %s (%ddBm, %ds sleep)\n",
                profileToName(profile),
                currentMode->lora_power_dbm,
                currentMode->sleep_interval_s);
}
```

### Sending a Binary Mode ACK

```cpp
void sendModeAck(uint32_t cmdMsgSeq)
{
  uint8_t buf[BP_MAX_PACKET_SIZE];

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(),
           STATUS_OK, PKT_MODE_ACK);

  pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
  pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentMode->lora_power_dbm);
  pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentMode->sleep_interval_s);
  pkt_add_tlv_u32(buf, TLV_CMD_MSG_ID,     cmdMsgSeq);  // Echo back!

  uint8_t len = pkt_finalize(buf);
  int state = radio.transmit(buf, len);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("ACK sent (%d bytes, echoing msg_seq=%lu)\n", len, cmdMsgSeq);
  } else {
    Serial.printf("ACK TX failed: %d\n", state);
  }
}
```

### Sending a Binary Status Response

```cpp
void sendStatusResponse(uint32_t cmdMsgSeq)
{
  uint8_t buf[BP_MAX_PACKET_SIZE];

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(),
           STATUS_OK, PKT_STATUS_RESP);

  uint16_t battMv = readBatteryMv();
  memcpy(&buf[22], &battMv, 2);

  pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
  pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentMode->lora_power_dbm);
  pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentMode->sleep_interval_s);
  pkt_add_tlv_u8(buf,  TLV_GPS_WARM,       gpsWarm ? 1 : 0);
  pkt_add_tlv_u8(buf,  TLV_HOME_CYCLES,    bleHomeCycles);
  pkt_add_tlv_u32(buf, TLV_CMD_MSG_ID,     cmdMsgSeq);  // Echo back!

  uint8_t len = pkt_finalize(buf);
  radio.transmit(buf, len);
}
```

### Sending a Lost Mode Timeout Alert

```cpp
void sendLostModeTimeoutAlert()
{
  uint8_t buf[BP_MAX_PACKET_SIZE];

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(),
           STATUS_LOST_TIMEOUT, PKT_ALERT);

  pkt_add_tlv_u8(buf,  TLV_NEW_MODE,   PROFILE_ACTIVE);
  pkt_add_tlv_u32(buf, TLV_DURATION_S, LOST_MODE_MAX_DURATION_S);

  uint8_t len = pkt_finalize(buf);
  radio.transmit(buf, len);

  Serial.println("Lost mode timeout alert sent");
}
```

### Listening for Commands

After each telemetry transmission, open a 2-second receive window:

```cpp
void listenForCommands()
{
  Serial.println("Listening for commands (2s window)...");
  uint32_t listenStart = millis();

  while (millis() - listenStart < 2000) {
    uint8_t rxBuf[BP_MAX_PACKET_SIZE];
    int state = radio.receive(rxBuf, BP_MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      uint8_t rxLen = radio.getPacketLength();

      // Validate: minimum size, version byte, CRC
      if (rxLen < BP_MIN_PACKET_SIZE) break;
      if (rxBuf[0] != BP_PROTOCOL_VERSION) break;
      if (!pkt_validate_crc(rxBuf, rxLen)) {
        Serial.println("CRC failed - ignoring");
        break;
      }

      // Device filter
      uint16_t target = pkt_device_id(rxBuf);
      if (target != MY_DEVICE_ID && target != DEVICE_ID_BROADCAST) {
        Serial.printf("Not for me (0x%04X) - ignoring\n", target);
        break;
      }

      // Dispatch
      uint32_t cmdMsgSeq = pkt_msg_seq(rxBuf);
      uint16_t ptype = pkt_pkt_type(rxBuf);

      if (ptype == PKT_CMD_MODE) {
        uint8_t profileEnum;
        if (pkt_tlv_get_u8(rxBuf, TLV_PROFILE, &profileEnum)) {
          applyProfile((bp_profile_t)profileEnum);
          sendModeAck(cmdMsgSeq);
        }
      } else if (ptype == PKT_CMD_STATUS) {
        sendStatusResponse(cmdMsgSeq);
      }

      break;  // One command per listen window
    }

    delay(10);
  }
}
```

### Lost Mode Timeout Check in loop()

```cpp
void loop()
{
  // Check lost mode timeout
  if (lostModeStartTime > 0) {
    uint32_t elapsedMs = millis() - lostModeStartTime;
    uint32_t elapsedSecs = elapsedMs / 1000;

    if (elapsedSecs >= LOST_MODE_MAX_DURATION_S) {
      Serial.println("Lost mode timeout - reverting to Active");
      sendLostModeTimeoutAlert();
      applyProfile(PROFILE_ACTIVE);  // Resets lostModeStartTime to 0
    }
  }

  // ... rest of loop (transmit cycle, sleep, etc.) ...
}
```

---

## Using Current Mode Parameters

### TX Power

```cpp
radio.setOutputPower(currentMode->lora_power_dbm);
```

### Sleep Interval

```cpp
esp_sleep_enable_timer_wakeup(currentMode->sleep_interval_s * 1000000ULL);
esp_deep_sleep_start();
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

## Minimal Complete Example

```cpp
#include <protocol.h>
#include <config.h>
#include <RadioLib.h>

#define MY_DEVICE_ID 0x0004  // Podge

const OperatingMode *currentMode = &MODE_NORMAL;
bp_profile_t currentProfile = PROFILE_NORMAL;
uint32_t lostModeStartTime = 0;
uint32_t mySeqCounter = 1;
bool gpsWarm = false;
uint8_t bleHomeCycles = 0;

SX1276 radio = /* your radio init */;

void setup()
{
  Serial.begin(115200);
  radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
              LORA_SYNC_WORD, currentMode->lora_power_dbm, LORA_PREAMBLE);
}

void loop()
{
  // Lost mode timeout check
  if (lostModeStartTime > 0 &&
      (millis() - lostModeStartTime) / 1000 >= LOST_MODE_MAX_DURATION_S) {
    sendLostModeTimeoutAlert();
    applyProfile(PROFILE_ACTIVE);
  }

  // Wake -> GPS -> select comm path -> transmit
  bool hasGps = getGpsFix();
  uint8_t commPath = selectCommPath();

  if (commPath == COMM_PATH_CELLULAR) {
    notifyHubSwitchingToCellular();  // Last LoRa with FLAG_CELLULAR_UPLINK
    sendCellularTelemetry(/* GPS params */);
  } else {
    sendLoRaTelemetry(hasGps, lat, lon, accM, speedCms, distHomeM, bearing,
                      readBatteryMv(), fixAgeS,
                      hasGps ? STATUS_OUT_AND_ABOUT : STATUS_INVALID_GPS,
                      /* switchingToCellular= */ false);
  }

  // Listen for Hub commands (LoRa path only — no downlink on cellular)
  if (commPath == COMM_PATH_LORA) {
    listenForCommands();
  }

  // LED flashes
  for (uint8_t i = 0; i < currentMode->led_flash_count; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }

  // Sleep
  esp_sleep_enable_timer_wakeup(currentMode->sleep_interval_s * 1000000ULL);
  esp_deep_sleep_start();
}
```

---

## Integration Checklist

**Protocol files:**
- [ ] Copy `protocol.h` to transmitter project (must be IDENTICAL to Hub's)
- [ ] Copy `config.h` to transmitter project (must be IDENTICAL to Hub's)
- [ ] Set `MY_DEVICE_ID` matching `DEVICE_REGISTRY` in `protocol.h`

**LoRa primary path:**
- [ ] Replace JSON telemetry with `sendLoRaTelemetry()` (binary PKT_TELEMETRY)
- [ ] Include `TLV_COMM_PATH = COMM_PATH_LORA` in every LoRa packet
- [ ] Implement `applyProfile()` to update `currentMode` and `currentProfile`
- [ ] Implement `sendModeAck()` with `TLV_CMD_MSG_ID` echo
- [ ] Implement `sendStatusResponse()` with `TLV_CMD_MSG_ID` echo
- [ ] Implement `sendLostModeTimeoutAlert()` (PKT_ALERT)
- [ ] Add `listenForCommands()` after each telemetry transmission
- [ ] Add lost mode timeout check in `loop()`
- [ ] Use `currentMode->sleep_interval_s` for deep sleep timer
- [ ] Use `currentMode->lora_power_dbm` for TX power
- [ ] Use `currentMode->led_flash_count` for post-TX LED flashes
- [ ] Handle `currentMode->led_beacon_mode` in Lost mode

**Cellular fallback path:**
- [ ] Implement `selectCommPath()` to decide LoRa vs cellular each cycle
- [ ] Send last LoRa packet with `FLAG_CELLULAR_UPLINK` set before switching to cellular
- [ ] Implement `sendCellularTelemetry()` via Cat-1/NB-IoT modem (cloud-direct)
- [ ] Track `consecutiveLoRaMisses` to trigger cellular fallback in Lost mode
- [ ] On returning to LoRa path, resume `listenForCommands()` to catch queued Hub commands

---

## Testing Sequence

**LoRa primary path (normal operation):**
1. **Flash updated collar** with `protocol.h`, `config.h`, and binary packet code
2. **Power on collar** — Serial should show binary LoRa telemetry being sent
3. **Open Serial monitor on Home Hub** — look for `[LORA] Binary packet received`
4. **Open Command & Control panel** in web UI — collar should appear in node list
5. **Change mode to ACTIVE** — collar should ACK within 2 transmit cycles
6. **Check mode badge** — should update to green "ACTIVE"
7. **Query status** — should receive PKT_STATUS_RESP with battery, GPS warm, etc.
8. **Test LOST mode** — confirmation dialog, countdown timer, high TX power
9. **Wait for timeout** (or reduce `LOST_MODE_MAX_DURATION_S` for testing) — should revert to ACTIVE

**Cellular fallback path:**
10. **Simulate Hub unavailable** (e.g., power off Hub) — collar should hit `LORA_MISS_THRESHOLD`
11. **Verify `FLAG_CELLULAR_UPLINK` set** in final LoRa packet before switchover
12. **Verify cellular transmission** begins via Cat-1/NB-IoT modem (check AT command logs)
13. **Restore Hub** — collar should fall back to LoRa path on next cycle
14. **Verify Hub picks up queued mode change** (if any) via first LoRa listen window

---

## Debugging

### Collar Serial Should Show (LoRa Path)

```
Waking up...
GPS locked (warm start)
Comm path: LoRa -> Home Hub -> Cloud
LoRa telemetry sent (47 bytes) -> Home Hub -> Cloud
Listening for commands (2s window)...
Binary packet received (41 bytes)
CRC OK, device_id match (0x0004)
PKT_CMD_MODE -> PROFILE_ACTIVE
Profile applied: active (19dBm, 60s sleep)
ACK sent (53 bytes, echoing msg_seq=42)
Going to sleep (60s)...
```

### Collar Serial Should Show (Cellular Fallback)

```
Waking up...
GPS locked (warm start)
Comm path: Cellular (LoRa miss count=3, threshold=3)
Sending final LoRa packet with FLAG_CELLULAR_UPLINK...
LoRa telemetry sent (47 bytes) [last LoRa, switching to cellular]
Cellular telemetry sent -> Cloud (bypassing Home Hub)
Going to sleep (30s)...
```

### Home Hub Serial Should Show

```
[CMD] Queued for Podge (msg_id=42, 41 bytes)
[PKT] 41 bytes: 01 04 00 2A 00 00 00 ...
[LoRa] Command transmitted (msg_id=42)
[LORA] Binary packet received (53 bytes, RSSI=-45, SNR=9.5)
[ACK] Podge confirmed mode: active (msg_id=42, 19dBm, 60s)
WebSocket: Broadcasting node states
```

When collar switches to cellular, Hub logs:
```
[LORA] Binary packet received — FLAG_CELLULAR_UPLINK set (Podge switching to cellular path)
[INFO] Podge will not be reachable via LoRa while on cellular path
```

### Common Issues

**Hub logs "CRC validation failed":**

- Collar firmware is probably still sending JSON (first byte is `{` not `0x01`)
- Or byte order mismatch — ensure both sides include the same `protocol.h`

**Hub logs "Unknown binary packet type":**

- `flags` field is wrong — check `PKT_CMD_MODE` constant value matches in both files

**No commands received by collar:**

- Verify listen window runs immediately after `sendLoRaTelemetry()`
- Check `BP_MIN_PACKET_SIZE` and `BP_PROTOCOL_VERSION` match between projects
- Verify LoRa parameters (frequency, SF, BW, CR) are identical on both devices
- If collar is on cellular path, it won't receive LoRa commands — wait for LoRa cycle

**ACK received but msg_id doesn't match:**

- Check `TLV_CMD_MSG_ID` is being added via `pkt_add_tlv_u32()` and extracted via `pkt_tlv_get_u32()`
- Ensure both sides use `uint32_t` for the sequence counter

**Lost mode doesn't auto-revert:**

- Check `lostModeStartTime` is set in `applyProfile()` when `PROFILE_LOST`
- Verify timeout check runs every `loop()` iteration
- Confirm `LOST_MODE_MAX_DURATION_S` is defined in `config.h`

**Collar stuck on cellular path:**

- Check `consecutiveLoRaMisses` counter — should reset to 0 when a LoRa packet is received
- Verify Hub is powered on and within LoRa range
- Confirm LoRa radio is reinitialised correctly after cellular modem use

---

## Verification

Once implemented, you should see:

- Binary hex dumps in Serial (not JSON strings)
- All tracked collar nodes visible in Command & Control panel
- `TLV_COMM_PATH` present in every telemetry packet
- Mode changes confirmed via PKT_MODE_ACK (TLV_CMD_MSG_ID matches sent msg_seq)
- Status responses include battery mV, GPS warm flag, home cycle count
- Countdown timer in UI during lost mode
- Automatic revert to Active mode after 2 hours, with PKT_ALERT sent
- `FLAG_CELLULAR_UPLINK` visible in final LoRa packet before cellular switchover
- Cellular telemetry arriving at cloud endpoint directly (Hub not involved)

---

**Check the Home Hub's `main.cpp` for the reference binary packet handling and ACK processing implementation.**

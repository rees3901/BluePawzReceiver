# Simplified Command & ACK Protocol

## System Context

```
Many collars -> one Home Hub -> Wi-Fi -> Cloud  (primary path)
Collar -> Cat-1/NB-IoT -> Cloud                (cellular fallback, Hub bypassed)
```

Commands travel **Hub -> Collar** via LoRa downlink only.
Telemetry travels **Collar -> Hub -> Cloud** via LoRa uplink + Wi-Fi.

---

## Flow Overview

```
User clicks "Send"
    |
Hub builds binary packet:
    pkt_init(buf, device_id=0x0004, msg_seq=42, ..., PKT_CMD_MODE)
    pkt_add_tlv_u8(buf, TLV_PROFILE, PROFILE_ACTIVE)
    pkt_finalize(buf)   ->  41 bytes including CRC-16
    |
LoRa downlink transmit (raw bytes, Hub -> Collar)
    |
Collar receives -> CRC check -> device_id match -> Apply mode
    |
Collar sends PKT_MODE_ACK:
    TLV_PROFILE + TLV_TX_POWER + TLV_SLEEP_INTERVAL + TLV_CMD_MSG_ID=42
    |
Hub confirms: Command acknowledged (msg_id=42 matched)
    |
Next telemetry (PKT_TELEMETRY) includes current mode in TLVs
```

---

## Why Binary TLV?

**Old approach (JSON):**

```
{"cmd":"mode","profile":"active","device":"Podge","msg_id":42}
= 62 bytes, no integrity check, ArduinoJson heap allocation
```

**Binary TLV:**

```
01 04 00 2A 00 00 00 00 00 00 00 04 05 00 [GPS zeroes...] 03 00 01 01 03 [CRC]
= 41 bytes, CRC-16 protected, direct memcpy parsing
```

**Benefits:**

- ~35% smaller packets — less LoRa airtime, better battery life
- CRC-16/CCITT-FALSE — corrupt packets detected and silently dropped
- Faster parsing — no JSON allocation on constrained MCU
- Extensible — new TLV types added without breaking existing parsers (unknown IDs skipped)
- Device targeting in header — no string comparison needed

---

## Collar Implementation

### Receiving a Binary Command

```cpp
void handleBinaryCommand(const uint8_t *buf, uint8_t pkt_len)
{
  // device_id check already done by caller
  uint16_t ptype = pkt_pkt_type(buf);
  uint32_t cmdMsgSeq = pkt_msg_seq(buf);  // Save for ACK

  if (ptype == PKT_CMD_MODE) {
    uint8_t profileEnum;
    if (pkt_tlv_get_u8(buf, TLV_PROFILE, &profileEnum)) {
      applyProfile((bp_profile_t)profileEnum);
      Serial.printf("Mode changed to: %s\n",
                    profileToName((bp_profile_t)profileEnum));
      sendModeAck(cmdMsgSeq);
    }
  }
  else if (ptype == PKT_CMD_STATUS) {
    sendStatusResponse(cmdMsgSeq);
  }
}
```

### Simple Mode ACK

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
  radio.transmit(buf, len);
  Serial.printf("ACK sent (%d bytes, msg_seq=%lu)\n", len, cmdMsgSeq);
}
```

### Include Mode in Telemetry Updates

```cpp
void sendTelemetry()
{
  uint8_t buf[BP_MAX_PACKET_SIZE];
  uint16_t flags = PKT_TELEMETRY;
  if (gpsValid) flags |= FLAG_HAS_GPS;
  if (bleHome)  flags |= FLAG_BLE_HOME;

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(),
           currentStatus, flags);

  if (gpsValid) {
    pkt_set_gps(buf, lat_e7, lon_e7, distHomeM, bearingDeg);
    // Also set batt_mV, acc_m, speed_cms in header bytes 22-29
  }

  // Current mode info — always included
  pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
  pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentMode->lora_power_dbm);
  pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentMode->sleep_interval_s);

  uint8_t len = pkt_finalize(buf);
  radio.transmit(buf, len);
}
```

---

## Sleeping Collar Scenario

### Problem

Collar is asleep when command arrives — can't receive it immediately.

### Solution

Collar listens for binary commands for 2 seconds after each GPS transmission.

```cpp
void transmitCycle()
{
  // 1. Wake up
  // 2. Get GPS fix
  // 3. Send binary telemetry (includes current mode in TLVs)
  sendTelemetry();

  // 4. Listen for binary commands for 2 seconds
  uint32_t listenStart = millis();
  while (millis() - listenStart < 2000) {
    uint8_t rxBuf[BP_MAX_PACKET_SIZE];
    uint8_t rxLen = 0;
    int state = radio.receive(rxBuf, BP_MAX_PACKET_SIZE);

    if (state == RADIOLIB_ERR_NONE) {
      rxLen = radio.getPacketLength();

      // Validate: version, CRC, device_id
      if (rxLen >= BP_MIN_PACKET_SIZE &&
          rxBuf[0] == BP_PROTOCOL_VERSION &&
          pkt_validate_crc(rxBuf, rxLen)) {

        uint16_t target = pkt_device_id(rxBuf);
        if (target == MY_DEVICE_ID || target == DEVICE_ID_BROADCAST) {
          handleBinaryCommand(rxBuf, rxLen);
          break;  // Processed, exit listen window
        }
      }
    }
    delay(10);
  }

  // 5. Sleep using current mode interval
  esp_sleep_enable_timer_wakeup(currentMode->sleep_interval_s * 1000000ULL);
  esp_deep_sleep_start();
}
```

### Timeline Example

```
00:00 - Collar asleep (Normal mode, 5min interval)
02:30 - User sends "change to Active"
02:30 - Hub queues binary PKT_CMD_MODE (msg_seq=42, 41 bytes)
02:30 - Hub transmits via LoRa; collar is asleep -> missed

05:00 - Collar wakes
05:05 - Collar sends PKT_TELEMETRY (TLV_PROFILE=PROFILE_NORMAL) -> Hub -> Cloud
05:05 - Collar enters 2-second listen window
05:05 - Hub retransmits PKT_CMD_MODE (msg_seq=42) via LoRa downlink
05:05 - Collar: CRC OK, device_id match -> PROFILE_ACTIVE applied
05:05 - Collar sends PKT_MODE_ACK (TLV_CMD_MSG_ID=42)
05:05 - Collar goes back to sleep

06:05 - Collar wakes (Active mode, 1min interval now)
06:06 - Collar sends PKT_TELEMETRY (TLV_PROFILE=PROFILE_ACTIVE) -> Hub -> Cloud
06:06 - Hub sees mode confirmed in telemetry TLVs
```

---

## Home Hub Behaviour

### ACK Handling

```cpp
void handleBinaryModeAck(const uint8_t *buf, uint8_t pkt_len,
                         int16_t rssi, float snr)
{
  uint16_t deviceId = pkt_device_id(buf);
  const char *name  = getDeviceName(deviceId);

  uint8_t profileEnum = PROFILE_UNKNOWN;
  pkt_tlv_get_u8(buf, TLV_PROFILE, &profileEnum);

  int8_t txPower = 0;
  pkt_tlv_get_i8(buf, TLV_TX_POWER, &txPower);

  uint16_t sleepInterval = 0;
  pkt_tlv_get_u16(buf, TLV_SLEEP_INTERVAL, &sleepInterval);

  uint32_t cmdMsgId = 0;
  pkt_tlv_get_u32(buf, TLV_CMD_MSG_ID, &cmdMsgId);

  Serial.printf("[ACK] %s confirmed mode: %s (msg_id=%lu)\n",
                name, profileToName((bp_profile_t)profileEnum), cmdMsgId);

  updateNodeFromAck(name, profileEnum, txPower, sleepInterval);
  broadcastNodeStates();  // Push to WebSocket clients
}
```

---

## UI Feedback Flow

### 1. User Clicks Send

```javascript
// Button disabled, shows pending state
cardModeText.innerHTML = '<span style="color: #ffc107;">Sending command...</span>';
```

### 2. Binary Command Transmitted

```
Serial: [LoRa] Transmitting binary command to Podge (msg_id=42, 41 bytes)
Serial: [PKT] 41 bytes: 01 04 00 2A 00 00 00 00 00 00 00 04 05 00 ...
```

### 3. Binary ACK Received

```
Serial: [LORA] Binary packet received (53 bytes, RSSI=-42, SNR=10.0)
Serial: [ACK] Podge confirmed mode: active (msg_id=42)
// WebSocket broadcasts updated node state -> UI re-enables button
```

### 4. Next Telemetry Update

```
Serial: [LORA] Binary packet received (PKT_TELEMETRY, Podge, TLV_PROFILE=active)
// UI mode badge updates to "ACTIVE" (green)
```

---

## Serial Monitor Examples

### Home Hub

```
[CMD] Queued for Podge (msg_id=42, 41 bytes)
[PKT] 41 bytes: 01 04 00 2A 00 00 00 00 00 00 00 04 05 00 00 00 00 00 ...
[LoRa] Command transmitted (msg_id=42)
[LORA] Binary packet received (53 bytes, RSSI=-45, SNR=9.2)
[ACK] Podge confirmed mode: active (msg_id=42, 19dBm, 60s)
WebSocket: Broadcasting node states
```

### Collar (Podge)

```
Waking up...
GPS locked (warm start)
Sending binary telemetry (47 bytes)  [LoRa -> Home Hub -> Cloud]
Listening for commands (2s window)...
Binary packet received (41 bytes)
CRC OK, device_id match (0x0004)
PKT_CMD_MODE -> PROFILE_ACTIVE
Mode changed to: active
ACK sent (53 bytes, msg_seq=42)
Going to sleep (Active mode, 60s)...
```

### Collar (Macy — ignoring)

```
Binary packet received (41 bytes)
CRC OK
Not for me (target=0x0004, my_id=0x0001) - IGNORING
```

---

## Key Points

- **Binary packets are ~35% smaller** than equivalent JSON — less airtime, better battery
- **CRC-16 protects every packet** — corrupt packets silently dropped, no bad data processed
- **Device ID in header bytes 1-2** — filtering before any TLV parsing
- **TLV_CMD_MSG_ID echoes msg_seq** — reliable command-to-ACK matching
- **TLVs are optional and order-independent** — parsers skip unknown type IDs safely
- **Legacy JSON still accepted** on RX side — detected by first byte `{` (0x7B) vs `0x01`
- **Mode always in telemetry TLVs** — Hub confirms mode from regular uplink updates too
- **No command path over cellular** — if collar is on Cat-1/NB-IoT fallback, Hub cannot reach it via LoRa; mode changes apply on next LoRa-path cycle

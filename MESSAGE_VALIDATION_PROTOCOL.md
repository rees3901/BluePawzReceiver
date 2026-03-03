# Message Validation Protocol

## Overview

The binary protocol (defined in `protocol.h`) provides built-in **device targeting** and **message tracking** at the packet header level, replacing the JSON field-injection approach.

---

## Why Message Validation?

**Problems Solved:**

1. **Multiple nodes** - With 5 trackers (Podge, Macy, Gizmo, Simba, Carrie), each command must reach only the right device
2. **Command confirmation** - Base station needs to know which specific command was acknowledged
3. **Message tracking** - Prevents confusion when multiple commands are in-flight
4. **Integrity** - Corrupt packets (noise, partial RX) are detected and dropped

---

## How the Binary Protocol Handles It

### Device Targeting: Header Bytes 1-2

Every packet carries `device_id` (u16, little-endian) in the header. TX nodes check this field before processing — if it doesn't match `MY_DEVICE_ID`, the packet is ignored entirely.

```
Byte 0:   0x01          (protocol version)
Bytes 1-2: 04 00        (device_id = 0x0004 = Podge, little-endian)
Bytes 3-6: 2A 00 00 00  (msg_seq = 42, little-endian)
...
```

Device registry (defined in `protocol.h`):

| ID | Name |
|----|------|
| 0x0000 | Base Station (RX) |
| 0x0001 | Macy |
| 0x0002 | Gizmo |
| 0x0003 | Simba |
| 0x0004 | Podge |
| 0x0005 | Carrie |
| 0xFFFF | Broadcast (all collars) |

Resolved by `getDeviceIdByName()` and `getDeviceName()` in `protocol.h`.

### Message Tracking: Header Bytes 3-6

Every packet carries `msg_seq` (u32, little-endian) — a monotonically incrementing counter assigned by the sender.

When a TX node ACKs a command, it echoes the original command's `msg_seq` back in a TLV:

```
TLV_CMD_MSG_ID (0x0B): u32 = <msg_seq from command packet>
```

This lets the base station unambiguously match ACKs to the commands that triggered them.

### Integrity: CRC-16/CCITT-FALSE (Bytes after TLV)

Every packet is protected by CRC-16 (polynomial 0x1021, init 0xFFFF) computed over all header + TLV bytes, appended as the final 2 bytes. The receiver calls `pkt_validate_crc()` before dispatching — corrupt or truncated packets are silently dropped.

---

## Protocol Structure

### Base Station -> TX Node (Mode Change Command: PKT_CMD_MODE)

```
Header (36 bytes):
  [0]     0x01              version
  [1-2]   04 00             device_id = 0x0004 (Podge)
  [3-6]   2A 00 00 00       msg_seq = 42
  [7-10]  00 00 00 00       time_unix = 0 (not needed for commands)
  [11]    04                status = STATUS_OK
  [12-13] 05 00             flags = PKT_CMD_MODE (0x0005)
  [14-33] 00 ...            GPS fields unused for commands
  [34]    03                tlv_len = 3 bytes
  [35]    00                reserved

TLV payload (3 bytes):
  [36]    01                type = TLV_PROFILE
  [37]    01                length = 1
  [38]    04                value = PROFILE_LOST

CRC-16 (2 bytes):
  [39-40] XX XX

Total: 41 bytes
```

Compare to the old JSON equivalent:

```json
{"cmd":"mode","profile":"lost","device":"Podge","msg_id":42}
```

That was 62 bytes with no integrity check.

### TX Node -> Base Station (Mode ACK: PKT_MODE_ACK)

```
Header:
  device_id = 0x0004  (Podge)
  msg_seq   = <node's own counter>
  time_unix = <current unix timestamp>
  status    = STATUS_OK
  flags     = PKT_MODE_ACK (0x0002)

TLV payload:
  TLV_PROFILE        (0x01): u8  = PROFILE_LOST    (0x04)
  TLV_TX_POWER       (0x02): i8  = 22
  TLV_SLEEP_INTERVAL (0x03): u16 = 30
  TLV_CMD_MSG_ID     (0x0B): u32 = 42              <- echoes command's msg_seq

CRC-16
Total: 53 bytes
```

### TX Node -> Base Station (Status Response: PKT_STATUS_RESP)

```
Header:
  device_id = 0x0004  (Podge)
  status    = STATUS_OK
  flags     = PKT_STATUS_RESP (0x0003)
  batt_mV   = 3700            (header bytes 22-23)

TLV payload:
  TLV_PROFILE        (0x01): u8     = PROFILE_NORMAL
  TLV_TX_POWER       (0x02): i8     = 19
  TLV_SLEEP_INTERVAL (0x03): u16    = 300
  TLV_GPS_WARM       (0x04): u8     = 1
  TLV_HOME_CYCLES    (0x05): u8     = 3
  TLV_LOG_INFO       (0x06): u16+u16 = entries=128, size_kb=45
  TLV_CMD_MSG_ID     (0x0B): u32    = <original cmd msg_seq>

CRC-16
```

---

## Base Station Implementation

### Message Sequence Counter

```cpp
uint32_t nextMessageId = 1; // Global counter, increments for each command
```

### Command Queue with Binary Packets

```cpp
struct LoRaCommand
{
  String targetDevice;              // "Podge", "Macy", etc.
  uint8_t buf[BP_MAX_PACKET_SIZE];  // Pre-built binary packet
  uint8_t len;                      // Total packet length (after finalize)
  uint32_t timestamp;               // millis() when queued
  uint32_t messageId;               // msg_seq for tracking ACKs
};
```

### Building and Queuing a Mode Command

```cpp
void sendModeCommand(const String &deviceId, const String &profile)
{
  uint16_t targetDeviceId = getDeviceIdByName(deviceId.c_str());
  bp_profile_t profileEnum = profileFromName(profile.c_str());

  LoRaCommand cmd;
  cmd.targetDevice = deviceId;
  cmd.messageId = nextMessageId++;
  cmd.timestamp = millis();

  pkt_init(cmd.buf, targetDeviceId, cmd.messageId, 0, STATUS_OK, PKT_CMD_MODE);
  pkt_add_tlv_u8(cmd.buf, TLV_PROFILE, profileEnum);
  cmd.len = pkt_finalize(cmd.buf);  // Appends CRC-16, returns total length

  commandQueue.push_back(cmd);
  Serial.printf("[CMD] Queued for %s (msg_id=%lu, %d bytes)\n",
                deviceId.c_str(), cmd.messageId, cmd.len);
}
```

### Transmitting Commands

```cpp
void processCommandQueue()
{
  // ... 3-second rate limiting ...

  LoRaCommand cmd = commandQueue.front();
  commandQueue.erase(commandQueue.begin());

  pkt_print_hex(cmd.buf, cmd.len);  // Hex dump to Serial

  lora.standby();
  int state = lora.transmit(cmd.buf, cmd.len);  // Raw binary TX

  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] Command transmitted (msg_id=%lu)\n", cmd.messageId);
  }
}
```

### Receiving and Dispatching Packets

```cpp
void onLoRaReceive(uint8_t *rxBuf, uint8_t rxLen, int16_t rssi, float snr)
{
  if (rxLen >= 1 && rxBuf[0] == BP_PROTOCOL_VERSION) {
    // Binary packet
    if (!pkt_validate_crc(rxBuf, rxLen)) {
      Serial.println("[LORA] Binary CRC validation failed - dropping");
      return;
    }
    switch (pkt_pkt_type(rxBuf)) {
      case PKT_TELEMETRY:   handleBinaryTelemetry(rxBuf, rxLen, rssi, snr);  break;
      case PKT_MODE_ACK:    handleBinaryModeAck(rxBuf, rxLen, rssi, snr);    break;
      case PKT_STATUS_RESP: handleBinaryStatusResp(rxBuf, rxLen, rssi, snr); break;
      case PKT_ALERT:       handleBinaryAlert(rxBuf, rxLen, rssi, snr);      break;
    }
  } else if (rxLen >= 1 && rxBuf[0] == '{') {
    // Legacy JSON fallback
    String json = String((char *)rxBuf);
    handleLoRaPacketJSON(json);
  }
}
```

### Receiving Mode ACKs

```cpp
void handleBinaryModeAck(const uint8_t *buf, uint8_t pkt_len,
                         int16_t rssi, float snr)
{
  uint16_t deviceId = pkt_device_id(buf);
  const char *name = getDeviceName(deviceId);

  uint8_t profileEnum = PROFILE_UNKNOWN;
  pkt_tlv_get_u8(buf, TLV_PROFILE, &profileEnum);

  int8_t txPower = 0;
  pkt_tlv_get_i8(buf, TLV_TX_POWER, &txPower);

  uint16_t sleepInterval = 0;
  pkt_tlv_get_u16(buf, TLV_SLEEP_INTERVAL, &sleepInterval);

  uint32_t cmdMsgId = 0;
  pkt_tlv_get_u32(buf, TLV_CMD_MSG_ID, &cmdMsgId);

  Serial.printf("[ACK] %s confirmed mode: %s (msg_id=%lu, %ddBm, %ds)\n",
                name, profileToName((bp_profile_t)profileEnum),
                cmdMsgId, txPower, sleepInterval);

  updateNodeFromAck(name, profileEnum, txPower, sleepInterval);
  broadcastNodeStates();
}
```

---

## TX Node Implementation

### Device ID Constant

```cpp
// In your TX project's main.cpp or config
#define MY_DEVICE_ID 0x0004  // Podge — change per device
```

### Receiving and Filtering Commands

```cpp
void onLoRaReceive(uint8_t *rxBuf, uint8_t rxLen)
{
  if (rxLen < BP_MIN_PACKET_SIZE || rxBuf[0] != BP_PROTOCOL_VERSION) return;

  if (!pkt_validate_crc(rxBuf, rxLen)) {
    Serial.println("CRC failed - ignoring");
    return;
  }

  // Device filter — ignore packets not addressed to this collar
  uint16_t targetId = pkt_device_id(rxBuf);
  if (targetId != MY_DEVICE_ID && targetId != DEVICE_ID_BROADCAST) {
    Serial.printf("Not for me (target=0x%04X, my_id=0x%04X) - IGNORING\n",
                  targetId, MY_DEVICE_ID);
    return;
  }

  uint32_t cmdMsgSeq = pkt_msg_seq(rxBuf);  // Save for echoing in ACK
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
}
```

### Sending Mode ACK (with TLV_CMD_MSG_ID echo)

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

  Serial.printf("ACK sent (%d bytes, echoing msg_seq=%lu)\n", len, cmdMsgSeq);
}
```

### Sending Status Response

```cpp
void sendStatusResponse(uint32_t cmdMsgSeq)
{
  uint8_t buf[BP_MAX_PACKET_SIZE];

  pkt_init(buf, MY_DEVICE_ID, mySeqCounter++, getUnixTime(),
           STATUS_OK, PKT_STATUS_RESP);

  // Battery in header
  uint16_t battMv = readBatteryMv();
  memcpy(&buf[22], &battMv, 2);

  // TLVs
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

---

## Message Flow Example

### Scenario: Base station sends mode change to Podge

**1. Base Station Builds and Queues:**

```
[CMD] Queued for Podge (msg_id=42, 41 bytes)
[PKT] 41 bytes: 01 04 00 2A 00 00 00 00 00 00 00 04 05 00 ... 01 01 04 XX XX
```

**2. Base Station Transmits:**

```
[LoRa] Transmitting binary command to Podge (msg_id=42, 41 bytes)
[LoRa] Command transmitted successfully
```

**3. Podge Receives (other cats ignore):**

```
Macy:  device_id=0x0004, my_id=0x0001 -> IGNORING
Gizmo: device_id=0x0004, my_id=0x0002 -> IGNORING
Podge: device_id=0x0004, my_id=0x0004 -> CRC OK -> PKT_CMD_MODE
  TLV_PROFILE = PROFILE_LOST -> applying
  Sending PKT_MODE_ACK with TLV_CMD_MSG_ID=42
```

**4. Base Station Receives ACK:**

```
[LORA] Binary packet received (53 bytes, RSSI=-45, SNR=9.5)
[ACK] Podge confirmed mode: lost (msg_id=42, 22dBm, 30s)
WebSocket: Broadcasting node states
```

---

## Benefits of Binary over JSON

| Aspect | Old JSON | Binary TLV |
|--------|----------|------------|
| Mode command size | ~62 bytes | ~41 bytes |
| Mode ACK size | ~45 bytes | ~53 bytes (more data!) |
| Status response | ~95 bytes | ~66 bytes |
| Device targeting | String field `"device":"Podge"` | Header u16 `device_id` |
| Message tracking | JSON field `"msg_id"` | Header u32 `msg_seq` + TLV_CMD_MSG_ID |
| Integrity check | None | CRC-16/CCITT-FALSE |
| Parse cost | ArduinoJson heap allocation | Direct memcpy field access |
| Unknown fields | Silently accepted | Unknown TLV IDs skipped |

---

## Testing Checklist

- [ ] Base station assigns unique msg_seq to each command
- [ ] Binary packets carry correct device_id in bytes 1-2
- [ ] TX node checks device_id before processing (other devices ignore)
- [ ] TX node echoes msg_seq via TLV_CMD_MSG_ID in ACKs and status responses
- [ ] Base station logs ACK with matching msg_id
- [ ] CRC validation passes for valid packets
- [ ] CRC validation rejects corrupt/noise packets (test by injecting a bad byte)
- [ ] Serial hex dump shows correct packet bytes
- [ ] Legacy JSON packets still handled (backward compatibility)
- [ ] WebSocket updates after ACK received

---

**Result:** Reliable, compact, CRC-protected multi-device command & control system.

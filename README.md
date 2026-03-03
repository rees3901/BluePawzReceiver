
# BluePawzReceiver

BluePawzReceiver is the **Home Hub concentrator** firmware for the BluePawz GPS cat tracking system. It runs on a Seeeduino ESP32S3 Xiao and acts as the central relay point: receiving telemetry from cat collars over LoRa (SX1262) and forwarding it to the cloud via the household Wi-Fi connection. A built-in web interface provides real-time mapping and command & control of all tracked collars.

---

## System Architecture

```
PRIMARY PATH (normal operation)

  [Cat Collar]  ---(SX1262 LoRa)---> [Home Hub]  ---(Wi-Fi)---> Cloud
  (SX1262 + GPS                       (this device)
   + Cat-1/NB-IoT)

FALLBACK PATH (collar-side, power-scheme driven, rare)

  [Cat Collar]  ---(Cat-1 / NB-IoT cellular)-------------------> Cloud
```

**Normal operation:** Each collar transmits position data via the SX1262 LoRa radio to the Home Hub. The Hub relays this data to the cloud over the household Wi-Fi connection. This keeps both battery usage and cellular data consumption to a minimum.

**Cellular fallback:** Each collar is also fitted with a Cat-1/NB-IoT modem. The collar's power scheme activates direct cellular uplink only in exceptional circumstances — for example, when out of LoRa range of the Hub, or when a specific power profile mandates it. Cellular transmissions bypass the Hub entirely and go straight to the cloud.

**Many-to-one topology:** Multiple collars (up to the registered device limit) all report to the single Home Hub. The Hub tracks each collar independently and broadcasts state updates to connected web clients via WebSocket.

---

## Key Features

- **LoRa concentrator:** Receives SX1262 LoRa telemetry from multiple cat collars simultaneously
- **Cloud relay:** Forwards collar data to the cloud over household Wi-Fi (no dedicated server hardware required)
- **Cellular-aware:** Understands that collars may occasionally bypass the Hub via built-in Cat-1/NB-IoT modem
- **Real-time mapping:** Leaflet.js web interface shows live cat positions on a map
- **Command & control:** Remotely change collar operating modes (Normal, Powersave, Active, Lost) via LoRa downlink
- **Binary TLV protocol:** Compact, CRC-protected binary packets over LoRa (replacing JSON) for efficiency and integrity
- **Many-to-one:** Handles all registered collars from a single Hub device
- **Cat-friendly collars:** Lightweight collar hardware with long battery life thanks to LoRa-first power scheme

---

## How It Works

1. **Cat collar wakes** on its sleep interval, acquires a GPS fix, and transmits a binary TLV telemetry packet via LoRa to the Home Hub.
2. **Home Hub receives** the LoRa packet, validates CRC, decodes it, updates the local node state, and relays the data to the cloud via Wi-Fi.
3. **Web interface** (served by the Hub on port 80) shows all collar positions on a live map and exposes a command & control panel.
4. **Commands** (mode changes, status queries) are sent from the Hub back to collars via LoRa downlink and acknowledged by the collar using the binary ACK protocol.
5. **Cellular fallback** activates on the collar side when dictated by the power scheme (e.g., hub unreachable, or Lost mode with no LoRa contact). The Hub is not involved in this path.

---

## Communication Paths

| Path | Radio | Direction | When Used |
|------|-------|-----------|-----------|
| LoRa uplink | SX1262 915 MHz | Collar -> Hub | Primary — all normal operation |
| LoRa downlink | SX1262 915 MHz | Hub -> Collar | Commands & mode changes |
| Wi-Fi relay | 2.4 GHz | Hub -> Cloud | After Hub receives LoRa data |
| Cellular uplink | Cat-1 / NB-IoT | Collar -> Cloud | Fallback only — hub bypassed |

---

## Operating Modes (Collar)

| Mode | Sleep | TX Power | Comm Path | Use Case |
|------|-------|----------|-----------|----------|
| **Normal** | 5 min | 19 dBm LoRa | LoRa -> Hub -> Cloud | Daily tracking |
| **Powersave** | 20 min | 10 dBm LoRa | LoRa -> Hub -> Cloud | Home / conserve battery |
| **Active** | 1 min | 19 dBm LoRa | LoRa -> Hub -> Cloud | Frequent monitoring |
| **Lost** | 30 sec | 22 dBm LoRa | LoRa -> Hub -> Cloud; cellular if no Hub ACK | Emergency search |

---

## Applications

- **Pet monitoring:** Track multiple cats from a single Home Hub
- **Power-efficient design:** LoRa-first approach keeps both battery life and data costs low
- **Cellular backup:** Cat-1/NB-IoT modem on collar ensures cloud connectivity even when out of Hub range

---

## Future Enhancements

- Cloud relay implementation (MQTT / HTTP push to backend service)
- Geofencing and historical route tracking
- Additional sensors (temperature, activity)
- Over-the-air firmware updates via cellular fallback channel

/*
  ┌─────────────────────────────────────────────────────────┐
  ║ 🐾 CAT TRACKER Receiver — LoRa Base + Web Server              ║
  ║ 🚁 Receives SX1262 LoRa + serves map via WiFi           ║
  ║ 🕜 Leaflet.js for live GPS display                      ║
  └─────────────────────────────────────────────────────────┘
*/

// ──────────────────────── LIBRARY INCLUDES ─────────────────────────
#include <Arduino.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <secrets.h> // Include your secrets.h file for WiFi credentials
#include <config.h>  // Shared configuration with TX nodes
#include "protocol.h"
#include <WiFi.h>
#include <WebServer.h>        // Include the WebServer library for HTTP server
#include <WebSocketsServer.h> // Include the WebSockets library for WebSocket server
#include <LittleFS.h>
#include <map>    // Include the map library
#include <vector> // Include for message log buffer
#include <TinyGPS++.h>
#include <ESPmDNS.h> // Add mDNS library
#include <ArduinoOTA.h> // V3: wireless firmware push from PlatformIO (espota)
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>

// ──────────────────────────── CONFIGURATION ───────────────────────────

WebServer server(80);
WebSocketsServer webSocket(81);
std::map<String, String> catPayloads;

// ───────────── LoRa Configuration — SX1262 via HSPI bus ───────────────
#define LORA_NSS 41
#define LORA_SCK 7
#define LORA_MOSI 9
#define LORA_MISO 8
#define LORA_RST 42
#define LORA_BUSY 40
#define LORA_DIO1 39
#define LORA_LED 48 // LoRa chip LED on GPIO 48

SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, LoRaSPI);

volatile bool packetReceived = false;

// ───────────── LED Blink Timer Config ─────────────
bool ledState = false;
unsigned long lastToggle = 0;
const unsigned long toggleInterval = 4000;

// ───────────── GPS Configuration ─────────────
#define GPS_RX D7
#define GPS_TX D6
#define GPS_BAUD 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial1(1);

// Add initial location JSON
JsonDocument deviceLocation;

// ───────────── Dynamic Home Location ─────────────
// V3: home location lives on the receiver only. Persisted to LittleFS so
// it survives reboots and can be changed from the web UI without reflashing.
// Collars no longer compute distance/bearing — the receiver does it on every
// inbound telemetry packet (handleLoRaPacketJSON) using the haversine via
// TinyGPSPlus::distanceBetween / courseTo.
#define HOME_LOCATION_FILE "/home_location.json"
float g_homeLat = 51.87378215701798f; // Default; overwritten by loadHomeLocation()
float g_homeLon = -2.239428653198173f;

static bool loadHomeLocation()
{
  if (!LittleFS.exists(HOME_LOCATION_FILE))
  {
    Serial.println("[HOME] No saved home location, using defaults");
    return false;
  }
  File f = LittleFS.open(HOME_LOCATION_FILE, "r");
  if (!f)
  {
    Serial.println("[HOME] Failed to open home_location.json for read");
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    Serial.printf("[HOME] Parse error: %s — falling back to defaults\n", err.c_str());
    return false;
  }
  if (doc["lat"].is<float>() && doc["lon"].is<float>())
  {
    g_homeLat = doc["lat"].as<float>();
    g_homeLon = doc["lon"].as<float>();
    Serial.printf("[HOME] Loaded: lat=%.6f lon=%.6f\n", g_homeLat, g_homeLon);
    return true;
  }
  Serial.println("[HOME] Missing lat/lon in file — using defaults");
  return false;
}

static bool saveHomeLocation(float lat, float lon)
{
  // Basic sanity check
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f)
  {
    Serial.printf("[HOME] Refusing to save out-of-range lat/lon: %.6f, %.6f\n", lat, lon);
    return false;
  }
  JsonDocument doc;
  doc["lat"] = lat;
  doc["lon"] = lon;
  File f = LittleFS.open(HOME_LOCATION_FILE, "w");
  if (!f)
  {
    Serial.println("[HOME] Failed to open home_location.json for write");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  g_homeLat = lat;
  g_homeLon = lon;
  Serial.printf("[HOME] Saved: lat=%.6f lon=%.6f\n", lat, lon);
  return true;
}

// Add a flag to track the serial connection state
bool serialPreviouslyOpened = false;

// Add WiFi connection status
bool isWiFiConnected = false;

// Track WebSocket clients
uint8_t connectedClients = 0;

// Add timer for device GPS updates
unsigned long lastDeviceGPSUpdateTime = 0;
const unsigned long DEVICE_GPS_UPDATE_INTERVAL = 10000; // 10 seconds in milliseconds

// ───────────── BLE Beacon Config ─────────────
#define BLE_DEVICE_NAME "CAT_TRACKER_HQ"
BLEAdvertising *pAdvertising = nullptr;
unsigned long lastBLEAdvertTime = 0;
const unsigned long BLE_ADVERT_INTERVAL = 3000; // 5 seconds
bool bleEnabled = true;                         // BLE beacon control flag

// ───────────── Message Logging Config ─────────────
#define LOG_FILE_PATH "/messages.json"
#define MAX_LOG_MESSAGES 500     // Circular buffer size
#define LOG_FLUSH_INTERVAL 60000 // Flush to file every 60 seconds
unsigned long lastLogFlushTime = 0;
std::vector<String> messageLogBuffer; // In-memory buffer
bool logFileInitialized = false;

// ───────────── Node State Tracking (Operating Modes) ─────────────
struct NodeState
{
  String deviceId;
  String currentMode = "unknown";
  int8_t txPower = 0;
  uint16_t sleepInterval = 0;
  uint32_t lastSeen = 0;          // millis() timestamp
  uint32_t lostModeStartTime = 0; // millis() when lost mode activated (0 = not in lost mode)
  bool modeKnown = false;
};

std::map<String, NodeState> nodeStates; // Track state of each TX node

// ───────────── LoRa Command Queue ─────────────
// V3 rollout: stays on JSON. Buffer sized for JSON command payloads.
// (Binary TLV path preserved on the `wip/binary-migration` branch.)
struct LoRaCommand
{
  uint8_t buf[256];                // JSON command buffer (was BP_MAX_PACKET_SIZE for binary)
  uint8_t len;                     // Packet length (JSON byte count, no null terminator)
  uint16_t targetDeviceId;         // Target device numeric ID (kept for logging; broadcast = 0xFFFF)
  String targetDevice;             // Device name (collar) or "broadcast"
  uint32_t timestamp;              // When command was queued
  uint32_t messageId;              // Unique message ID for tracking ACKs
};

std::vector<LoRaCommand> commandQueue;
unsigned long lastCommandTxTime = 0;
const unsigned long COMMAND_TX_INTERVAL = 3000; // 3 seconds between command transmissions
uint32_t nextMessageId = 1;                     // Global message ID counter

// Function declarations
void notifyClients();
void notifyPosition(const JsonDocument &doc);
void handleLoRaPacket();
void onReceive();
void setupGPS();
void handleDeviceOwnGPS();
void handleRoot();
void handleData();
void checkWiFiConnection();
void setupBLE();
void enableBLE();
void disableBLE();
void sendBleStateWS(uint8_t clientId = 255);
void handleWebSocketMessage(uint8_t num, uint8_t *payload, size_t length);
void LED_flicker();
void setup();
void loop();

// Message logging functions
String getGPSTimestamp();
void initMessageLog();
void logMessage(const JsonDocument &doc, const String &type);
void flushMessageLog();
void handleMessagesExport();
void handleClearLog();

// Node state and command functions
void updateNodeState(const JsonDocument &doc);
void processCommandQueue();
void sendModeCommand(const String &deviceId, const String &profile);
void sendStatusRequest(const String &deviceId);
void handleNodeResponse(const JsonDocument &doc);
void handleNodeStates();    // HTTP handler for /node-states
void handleSendCommand();   // HTTP handler for /send-command
void broadcastNodeStates(); // WebSocket broadcast of node states

// LED flicker function - 5 rapid flashes
void LED_flicker()
{
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(LORA_LED, HIGH);
    delay(50);
    digitalWrite(LORA_LED, LOW);
    delay(50);
  }
}

// ═════════════════════════════════════════════════════════════════════
// MESSAGE LOGGING FUNCTIONS
// ═════════════════════════════════════════════════════════════════════

// Get GPS-based ISO 8601 timestamp, fallback to millis() if GPS invalid
String getGPSTimestamp()
{
  if (gps.time.isValid() && gps.date.isValid())
  {
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(timestamp);
  }
  else
  {
    // Fallback to millis() if GPS time not valid
    return String("MILLIS_") + String(millis());
  }
}

// Initialize message log file
void initMessageLog()
{
  if (logFileInitialized)
    return;

  // Check if file exists
  if (!LittleFS.exists(LOG_FILE_PATH))
  {
    Serial.println("[LOG] Creating new message log file");
    File logFile = LittleFS.open(LOG_FILE_PATH, "w");
    if (logFile)
    {
      // Initialize with empty JSON array
      logFile.print("{\"device\":\"BluePawzReceiver\",\"messages\":[]}");
      logFile.close();
      Serial.println("[LOG] ✅ Message log file created");
    }
    else
    {
      Serial.println("[LOG] ❌ Failed to create message log file");
      return;
    }
  }
  else
  {
    Serial.println("[LOG] Message log file exists");
  }

  logFileInitialized = true;
}

// Add message to in-memory buffer
void logMessage(const JsonDocument &doc, const String &type)
{
  if (!logFileInitialized)
    return;

  // Create log entry with GPS timestamp
  JsonDocument logEntry;
  logEntry["timestamp"] = getGPSTimestamp();
  logEntry["gps_time_valid"] = gps.time.isValid() && gps.date.isValid();
  logEntry["type"] = type; // "lora", "mydevice", "event"

  // Copy all fields from original message
  JsonObjectConst sourceObj = doc.as<JsonObjectConst>();
  for (JsonPairConst kv : sourceObj)
  {
    logEntry[kv.key()] = kv.value();
  }

  // Serialize to string and add to buffer
  String jsonString;
  serializeJson(logEntry, jsonString);
  messageLogBuffer.push_back(jsonString);

  Serial.printf("[LOG] Buffered message (type=%s, buffer size=%d)\n",
                type.c_str(), messageLogBuffer.size());

  // If buffer is getting large, flush immediately
  if (messageLogBuffer.size() >= MAX_LOG_MESSAGES)
  {
    Serial.println("[LOG] ⚠️ Buffer full, flushing now...");
    flushMessageLog();
  }
}

// Flush in-memory buffer to LittleFS (circular buffer with max messages)
void flushMessageLog()
{
  if (messageLogBuffer.empty() || !logFileInitialized)
    return;

  Serial.printf("[LOG] Flushing %d messages to file...\n", messageLogBuffer.size());

  // Read existing log file
  File logFile = LittleFS.open(LOG_FILE_PATH, "r");
  if (!logFile)
  {
    Serial.println("[LOG] ❌ Failed to open log file for reading");
    return;
  }

  JsonDocument existingDoc;
  DeserializationError error = deserializeJson(existingDoc, logFile);
  logFile.close();

  if (error)
  {
    Serial.printf("[LOG] ❌ Failed to parse existing log: %s\n", error.c_str());
    // Recreate file if corrupted
    initMessageLog();
    messageLogBuffer.clear();
    return;
  }

  // Get existing messages array
  JsonArray messages = existingDoc["messages"].as<JsonArray>();

  // Add new messages from buffer
  for (const String &msgStr : messageLogBuffer)
  {
    JsonDocument msgDoc;
    if (deserializeJson(msgDoc, msgStr) == DeserializationError::Ok)
    {
      messages.add(msgDoc.as<JsonObject>());
    }
  }

  // Implement circular buffer - keep only last MAX_LOG_MESSAGES
  while (messages.size() > MAX_LOG_MESSAGES)
  {
    messages.remove(0); // Remove oldest message
  }

  // Write back to file
  logFile = LittleFS.open(LOG_FILE_PATH, "w");
  if (!logFile)
  {
    Serial.println("[LOG] ❌ Failed to open log file for writing");
    return;
  }

  serializeJson(existingDoc, logFile);
  logFile.close();

  Serial.printf("[LOG] ✅ Flushed successfully. Total messages in file: %d\n", messages.size());

  // Clear buffer
  messageLogBuffer.clear();
  lastLogFlushTime = millis();
}

// HTTP handler for exporting messages.json
void handleMessagesExport()
{
  // Flush any pending messages first
  flushMessageLog();

  if (!LittleFS.exists(LOG_FILE_PATH))
  {
    server.send(404, "text/plain", "Message log not found");
    return;
  }

  File logFile = LittleFS.open(LOG_FILE_PATH, "r");
  if (!logFile)
  {
    server.send(500, "text/plain", "Failed to open message log");
    return;
  }

  // Stream file to client with proper headers for download
  server.sendHeader("Content-Disposition", "attachment; filename=messages.json");
  server.streamFile(logFile, "application/json");
  logFile.close();

  Serial.println("[LOG] 📥 Message log exported to client");
}

// HTTP handler for clearing message log
void handleClearLog()
{
  // Flush any pending messages first
  flushMessageLog();

  // Delete the log file
  if (LittleFS.exists(LOG_FILE_PATH))
  {
    if (LittleFS.remove(LOG_FILE_PATH))
    {
      Serial.println("[LOG] 🗑️ Message log cleared");

      // Reinitialize the log file
      logFileInitialized = false;
      initMessageLog();

      server.send(200, "text/plain", "Message log cleared successfully");
    }
    else
    {
      Serial.println("[LOG] ❌ Failed to delete log file");
      server.send(500, "text/plain", "Failed to delete log file");
    }
  }
  else
  {
    Serial.println("[LOG] ⚠️ Log file does not exist");
    // Create new empty log file
    logFileInitialized = false;
    initMessageLog();
    server.send(200, "text/plain", "No log file to clear, created new empty log");
  }
}

// ═════════════════════════════════════════════════════════════════════
// NODE STATE TRACKING AND COMMAND FUNCTIONS
// ═════════════════════════════════════════════════════════════════════

// Update node state based on received message or ACK
void updateNodeState(const JsonDocument &doc)
{
  String deviceId = "";

  // Extract device ID from various possible fields
  if (doc["id"].is<String>())
  {
    deviceId = doc["id"].as<String>();
  }
  else if (doc["device"].is<String>())
  {
    deviceId = doc["device"].as<String>();
  }

  if (deviceId.isEmpty() || deviceId == "MyDevice")
  {
    return; // Don't track base station's own device
  }

  // Get or create node state
  NodeState &state = nodeStates[deviceId];
  state.deviceId = deviceId;
  state.lastSeen = millis();

  // Check if this is an ACK response
  if (doc["ack"].is<String>())
  {
    String ackType = doc["ack"].as<String>();

    // Extract and validate message_id
    uint32_t msgId = 0;
    if (doc["msg_id"].is<uint32_t>())
    {
      msgId = doc["msg_id"].as<uint32_t>();
      Serial.printf("[ACK] %s acknowledged command (msg_id=%lu) ✅\n",
                    deviceId.c_str(), msgId);
    }
    else
    {
      Serial.printf("[ACK] %s acknowledged (WARNING: no msg_id)\n",
                    deviceId.c_str());
    }

    // Handle mode ACK: extract profile, power, sleep
    if (ackType == "mode")
    {
      if (doc["profile"].is<String>())
      {
        state.currentMode = doc["profile"].as<String>();
        state.modeKnown = true;
      }
      if (doc["power"].is<int>())
      {
        state.txPower = doc["power"].as<int>();
      }
      if (doc["sleep"].is<int>())
      {
        state.sleepInterval = doc["sleep"].as<int>();
      }

      // Track lost mode activation
      if (state.currentMode == "lost" && state.lostModeStartTime == 0)
      {
        state.lostModeStartTime = millis();
      }
      else if (state.currentMode != "lost")
      {
        state.lostModeStartTime = 0; // Reset if exiting lost mode
      }

      Serial.printf("[NODE] %s confirmed mode: %s (Power: %ddBm, Sleep: %ds)\n",
                    deviceId.c_str(), state.currentMode.c_str(),
                    state.txPower, state.sleepInterval);
    }

    broadcastNodeStates();
    return; // ACK fully processed
  }
  // Check if this is a status response
  else if (doc["status"].is<String>() && doc["mode"].is<String>())
  {
    // Status query response
    state.currentMode = doc["mode"].as<String>();
    state.modeKnown = true;

    if (doc["power"].is<int>())
    {
      state.txPower = doc["power"].as<int>();
    }
    if (doc["sleep"].is<int>())
    {
      state.sleepInterval = doc["sleep"].as<int>();
    }

    if (doc["lost_mode_s"].is<int>())
    {
      // Lost mode is active, calculate start time
      uint32_t elapsedSecs = doc["lost_mode_s"].as<int>();
      state.lostModeStartTime = millis() - (elapsedSecs * 1000);
    }
    else
    {
      state.lostModeStartTime = 0;
    }

    Serial.printf("[NODE] %s status: mode=%s, power=%ddBm, sleep=%ds\n",
                  deviceId.c_str(), state.currentMode.c_str(),
                  state.txPower, state.sleepInterval);

    // Broadcast state update via WebSocket
    broadcastNodeStates();
  }
  // Check for lost mode timeout alert
  else if (doc["alert"].is<String>() && doc["alert"].as<String>() == "lost_mode_timeout")
  {
    Serial.printf("[NODE] ⚠️ %s lost mode timed out - auto-reverted to %s\n",
                  deviceId.c_str(), doc["new_mode"].as<String>().c_str());

    state.currentMode = doc["new_mode"].as<String>();
    state.lostModeStartTime = 0;
    state.modeKnown = true;

    // Broadcast alert via WebSocket
    JsonDocument alertDoc;
    alertDoc["type"] = "node_alert";
    alertDoc["device"] = deviceId;
    alertDoc["alert"] = "lost_mode_timeout";
    alertDoc["new_mode"] = doc["new_mode"].as<String>();
    alertDoc["duration_s"] = doc["duration_s"].as<int>();

    String alertJson;
    serializeJson(alertDoc, alertJson);
    webSocket.broadcastTXT(alertJson);

    broadcastNodeStates();
  }
}

// Transmit a single LoRaCommand right now and log success/failure.
// Bypasses queue lookup — caller is responsible for picking the cmd.
static void transmitCommand(const LoRaCommand &cmd)
{
  Serial.printf("[LoRa] Transmitting JSON command to %s (msg_id=%lu, %d bytes): %.*s\n",
                cmd.targetDevice.c_str(), cmd.messageId, cmd.len,
                cmd.len, (const char *)cmd.buf);

  lora.standby();
  int state = lora.transmit(cmd.buf, cmd.len);

  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.printf("[LoRa] Command transmitted successfully (msg_id=%lu)\n", cmd.messageId);
    LED_flicker();

    JsonDocument logDoc;
    logDoc["event"] = "command_sent";
    logDoc["target"] = cmd.targetDevice;
    logDoc["msg_id"] = cmd.messageId;
    logDoc["bytes"] = cmd.len;
    logMessage(logDoc, "event");
  }
  else
  {
    Serial.printf("[LoRa] Command transmission failed: %d (msg_id=%lu)\n",
                  state, cmd.messageId);

    JsonDocument logDoc;
    logDoc["event"] = "command_failed";
    logDoc["target"] = cmd.targetDevice;
    logDoc["msg_id"] = cmd.messageId;
    logDoc["error_code"] = state;
    logMessage(logDoc, "event");
  }

  lora.startReceive();
  lastCommandTxTime = millis();
}

// V3: opportunistic command send. Called the moment a telemetry packet arrives
// from a collar — we KNOW the collar is in its post-TX RX window, so anything
// queued for it goes out immediately. Sends at most ONE command per call to
// keep airtime fair; a burst is delivered across the next 3s extension window
// on the collar side via subsequent calls (or via processCommandQueue's normal
// path). Returns true if a command was transmitted.
//
// Bypasses the COMMAND_TX_INTERVAL gate because the timing case is the one
// we genuinely want — the collar is awake right now.
static bool transmitCommandForDevice(const String &reportingDevice)
{
  if (commandQueue.empty()) return false;

  // Find first queued command targeted to this device (or "broadcast")
  for (auto it = commandQueue.begin(); it != commandQueue.end(); ++it)
  {
    if (it->targetDevice == reportingDevice || it->targetDevice == "broadcast")
    {
      LoRaCommand cmd = *it;
      commandQueue.erase(it);
      Serial.printf("[LoRa] Opportunistic send: %s reported in, dispatching queued cmd msg_id=%lu\n",
                    reportingDevice.c_str(), cmd.messageId);
      transmitCommand(cmd);
      return true;
    }
  }
  return false;
}

// Process command queue and transmit via LoRa (JSON protocol).
// Safety-net path: handles broadcasts and acts as retry for any command
// that wasn't dispatched opportunistically (e.g. collar hasn't reported yet).
void processCommandQueue()
{
  // Rate limit command transmission (safety-net retry path)
  if (millis() - lastCommandTxTime < COMMAND_TX_INTERVAL)
  {
    return;
  }

  if (commandQueue.empty())
  {
    return;
  }

  LoRaCommand cmd = commandQueue.front();
  commandQueue.erase(commandQueue.begin());
  transmitCommand(cmd);
}

// Send mode change command to a specific node (JSON protocol for V3 rollout)
// Wire format: {"cmd":"mode","profile":"<name>","device":"<name>","msg_id":N}
// Transmitter expects this shape — see handleModeCommand() in BluePawzTransmitter.
void sendModeCommand(const String &deviceId, const String &profile)
{
  // Validate profile name
  const OperatingMode *mode = getModeByName(profile.c_str());
  if (mode == nullptr)
  {
    Serial.printf("[CMD] Invalid profile: %s\n", profile.c_str());
    return;
  }

  // Resolve target ID (informational/logging only — JSON uses the device name string)
  uint16_t targetId;
  if (deviceId == "broadcast")
  {
    targetId = DEVICE_ID_BROADCAST;
  }
  else
  {
    targetId = getDeviceIdByName(deviceId.c_str());
    if (targetId == 0)
    {
      Serial.printf("[CMD] Unknown device: %s\n", deviceId.c_str());
      return;
    }
  }

  // Build JSON command packet
  LoRaCommand cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.targetDevice = deviceId;
  cmd.targetDeviceId = targetId;
  cmd.timestamp = millis();
  cmd.messageId = nextMessageId++;

  JsonDocument doc;
  doc["cmd"] = "mode";
  doc["profile"] = profile;
  doc["device"] = deviceId; // collar matches against SENDER_ID or "broadcast"
  doc["msg_id"] = cmd.messageId;
  size_t written = serializeJson(doc, cmd.buf, sizeof(cmd.buf));
  cmd.len = (uint8_t)written;

  commandQueue.push_back(cmd);

  Serial.printf("[CMD] Mode change queued: %s -> %s (%u bytes JSON)\n",
                deviceId.c_str(), profile.c_str(), (unsigned)written);
}

// Send status request to a specific node (JSON protocol for V3 rollout)
// Wire format: {"cmd":"get_status","device":"<name>","msg_id":N}
void sendStatusRequest(const String &deviceId)
{
  // Resolve target ID (informational/logging only)
  uint16_t targetId;
  if (deviceId == "broadcast")
  {
    targetId = DEVICE_ID_BROADCAST;
  }
  else
  {
    targetId = getDeviceIdByName(deviceId.c_str());
    if (targetId == 0)
    {
      Serial.printf("[CMD] Unknown device: %s\n", deviceId.c_str());
      return;
    }
  }

  // Build JSON status request packet
  LoRaCommand cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.targetDevice = deviceId;
  cmd.targetDeviceId = targetId;
  cmd.timestamp = millis();
  cmd.messageId = nextMessageId++;

  JsonDocument doc;
  doc["cmd"] = "get_status";
  doc["device"] = deviceId;
  doc["msg_id"] = cmd.messageId;
  size_t written = serializeJson(doc, cmd.buf, sizeof(cmd.buf));
  cmd.len = (uint8_t)written;

  commandQueue.push_back(cmd);

  Serial.printf("[CMD] Status request queued for: %s (%u bytes JSON)\n",
                deviceId.c_str(), (unsigned)written);
}

// HTTP handler: Get node states as JSON
void handleNodeStates()
{
  JsonDocument doc;
  JsonArray nodes = doc.to<JsonArray>();

  for (auto &pair : nodeStates)
  {
    NodeState &state = pair.second;

    JsonObject node = nodes.add<JsonObject>();
    node["device"] = state.deviceId;
    node["mode"] = state.currentMode;
    node["power"] = state.txPower;
    node["sleep"] = state.sleepInterval;
    node["last_seen"] = state.lastSeen;
    node["mode_known"] = state.modeKnown;

    // Calculate lost mode remaining time if applicable
    if (state.lostModeStartTime > 0)
    {
      uint32_t elapsedMs = millis() - state.lostModeStartTime;
      uint32_t elapsedSecs = elapsedMs / 1000;
      uint32_t remainingSecs = 0;

      if (elapsedSecs < LOST_MODE_MAX_DURATION_S)
      {
        remainingSecs = LOST_MODE_MAX_DURATION_S - elapsedSecs;
      }

      node["lost_mode_elapsed_s"] = elapsedSecs;
      node["lost_mode_remaining_s"] = remainingSecs;
    }
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

// HTTP handler: Send command to node
void handleSendCommand()
{
  if (!server.hasArg("device") || !server.hasArg("action"))
  {
    server.send(400, "text/plain", "Missing parameters: device, action");
    return;
  }

  String deviceId = server.arg("device");
  String action = server.arg("action");

  if (action == "status")
  {
    sendStatusRequest(deviceId);
    server.send(200, "text/plain", "Status request queued");
  }
  else if (action == "mode")
  {
    if (!server.hasArg("profile"))
    {
      server.send(400, "text/plain", "Missing parameter: profile");
      return;
    }

    String profile = server.arg("profile");
    sendModeCommand(deviceId, profile);
    server.send(200, "text/plain", "Mode change command queued");
  }
  else
  {
    server.send(400, "text/plain", "Invalid action");
  }
}

// HTTP handler: GET /home — return current home lat/lon
void handleGetHome()
{
  JsonDocument doc;
  doc["lat"] = g_homeLat;
  doc["lon"] = g_homeLon;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// HTTP handler: POST /home — set new home lat/lon
// Accepts either form-encoded ?lat=&lon= or a JSON body {"lat":..,"lon":..}.
//
// Sanity check: the new home must be within HOME_MAX_DIST_FROM_RECEIVER_KM
// of the receiver's own GPS fix (if one is available). LoRa range to the
// collars caps around 20km in practice, so anything further is almost
// certainly a typo or a swapped lat/lon. Pass `force=1` to override.
void handleSetHome()
{
  static constexpr float HOME_MAX_DIST_FROM_RECEIVER_KM = 20.0f;

  float lat = 0.0f, lon = 0.0f;
  bool gotLat = false, gotLon = false;
  bool force = false;

  if (server.hasArg("lat") && server.hasArg("lon"))
  {
    lat = server.arg("lat").toFloat();
    lon = server.arg("lon").toFloat();
    gotLat = gotLon = true;
  }
  else if (server.hasArg("plain"))
  {
    // JSON body fallback
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok)
    {
      if (doc["lat"].is<float>())
      {
        lat = doc["lat"].as<float>();
        gotLat = true;
      }
      if (doc["lon"].is<float>())
      {
        lon = doc["lon"].as<float>();
        gotLon = true;
      }
      if (doc["force"].is<bool>())
      {
        force = doc["force"].as<bool>();
      }
    }
  }

  if (server.hasArg("force") && server.arg("force") == "1")
  {
    force = true;
  }

  if (!gotLat || !gotLon)
  {
    server.send(400, "text/plain", "Missing lat/lon (use ?lat=&lon= or JSON body)");
    return;
  }

  // Range sanity check against the receiver's own GPS fix.
  // If GPS has no valid fix yet, we accept the value (with a server log) —
  // we don't want to lock users out before the GPS has settled.
  if (!force && gps.location.isValid())
  {
    double recLat = gps.location.lat();
    double recLon = gps.location.lng();
    double dKm = TinyGPSPlus::distanceBetween(lat, lon, recLat, recLon) / 1000.0;
    if (dKm > HOME_MAX_DIST_FROM_RECEIVER_KM)
    {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Refused: new home is %.1f km from this base station — "
               "beyond LoRa range. Pass force=1 to override.",
               dKm);
      Serial.printf("[HOME] %s\n", msg);
      server.send(400, "text/plain", msg);
      return;
    }
  }
  else if (!force)
  {
    Serial.println("[HOME] No valid receiver GPS fix yet — skipping range check");
  }

  if (!saveHomeLocation(lat, lon))
  {
    server.send(400, "text/plain", "Invalid or unsavable lat/lon");
    return;
  }

  // Echo the saved values
  JsonDocument resp;
  resp["lat"] = g_homeLat;
  resp["lon"] = g_homeLon;
  resp["saved"] = true;
  String out;
  serializeJson(resp, out);
  server.send(200, "application/json", out);

  // Push to all WebSocket clients so the UI map updates immediately
  JsonDocument wsDoc;
  wsDoc["type"] = "home_location";
  wsDoc["lat"] = g_homeLat;
  wsDoc["lon"] = g_homeLon;
  String wsMsg;
  serializeJson(wsDoc, wsMsg);
  webSocket.broadcastTXT(wsMsg);
}

// Broadcast node states via WebSocket
void broadcastNodeStates()
{
  JsonDocument doc;
  doc["type"] = "node_states";

  JsonArray nodes = doc["nodes"].to<JsonArray>();

  for (auto &pair : nodeStates)
  {
    NodeState &state = pair.second;

    JsonObject node = nodes.add<JsonObject>();
    node["device"] = state.deviceId;
    node["mode"] = state.currentMode;
    node["power"] = state.txPower;
    node["sleep"] = state.sleepInterval;
    node["last_seen"] = state.lastSeen;
    node["mode_known"] = state.modeKnown;

    if (state.lostModeStartTime > 0)
    {
      uint32_t elapsedMs = millis() - state.lostModeStartTime;
      uint32_t elapsedSecs = elapsedMs / 1000;
      uint32_t remainingSecs = 0;

      if (elapsedSecs < LOST_MODE_MAX_DURATION_S)
      {
        remainingSecs = LOST_MODE_MAX_DURATION_S - elapsedSecs;
      }

      node["lost_mode_elapsed_s"] = elapsedSecs;
      node["lost_mode_remaining_s"] = remainingSecs;
    }
  }

  String output;
  serializeJson(doc, output);
  webSocket.broadcastTXT(output);
}

// Improve WebSocket notification with connection tracking

void onReceive()
{
  packetReceived = true;
  Serial.println("Receiving LoRa packet.."); // Debug log
}

// ═════════════════════════════════════════════════════════════════════
// SHARED HELPERS (used by JSON path and parked binary handlers)
// ═════════════════════════════════════════════════════════════════════

// Convert bearing degrees to cardinal direction string
static String cardinalFromDegrees(uint16_t deg)
{
  static const char *dirs[] = {
      "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  int idx = ((int)deg + 11) / 22 % 16;
  return String(dirs[idx]);
}

// ═════════════════════════════════════════════════════════════════════
// BINARY PROTOCOL HANDLERS (parked — see wip/binary-migration branch)
// ═════════════════════════════════════════════════════════════════════

#if 0 // V3 ROLLOUT: binary TLV handlers disabled. Parked on wip/binary-migration branch.
// Handle binary telemetry packet (TX->RX position / BLEHome / invalidGPS)
static void handleBinaryTelemetry(const uint8_t *buf, uint8_t pkt_len, int16_t rssi, float snr)
{
  uint16_t devId = pkt_device_id(buf);
  const char *devName = getDeviceName(devId);
  uint8_t status = pkt_status(buf);
  uint16_t flags = pkt_flags(buf);
  uint32_t msgSeq = pkt_msg_seq(buf);

  // Build JSON doc for compatibility with existing WebSocket/logging
  JsonDocument doc;
  doc["id"] = devName;
  doc["msg_id"] = msgSeq;
  doc["received_at"] = millis();
  doc["rssi"] = rssi;
  doc["snr"] = snr;

  // Map status enum to display string
  doc["status"] = statusToDisplayString((bp_status_t)status);

  if (flags & FLAG_HAS_GPS)
  {
    double lat = pkt_lat_e7(buf) / 1e7;
    double lon = pkt_lon_e7(buf) / 1e7;
    uint16_t dist = pkt_dist_home_m(buf);
    uint16_t bearing = pkt_bearing_deg(buf);

    doc["lat"] = lat;
    doc["lon"] = lon;
    doc["dist_home_m"] = dist;
    doc["bearing"] = String(bearing) + "-" + cardinalFromDegrees(bearing);
  }

  if (flags & FLAG_BLE_HOME)
  {
    doc["ble_home"] = true;
  }

  // Store payload for /data endpoint
  String payload;
  serializeJson(doc, payload);
  catPayloads[String(devName)] = payload;

  Serial.printf("[RX] Binary telemetry from %s (msg_id=%u, status=0x%02X)\n",
                devName, msgSeq, status);
  serializeJsonPretty(doc, Serial);
  Serial.println();

  logMessage(doc, "lora");
  updateNodeState(doc);
  notifyPosition(doc);
}

// Handle binary mode ACK (TX->RX mode change acknowledgement)
static void handleBinaryModeAck(const uint8_t *buf, uint8_t pkt_len, int16_t rssi, float snr)
{
  uint16_t devId = pkt_device_id(buf);
  const char *devName = getDeviceName(devId);

  JsonDocument doc;
  doc["id"] = devName;
  doc["device"] = devName;
  doc["received_at"] = millis();

  // Extract TLVs
  uint8_t profileEnum;
  if (pkt_tlv_get_u8(buf, TLV_PROFILE, &profileEnum))
  {
    doc["ack"] = "mode";
    doc["profile"] = profileToName((bp_profile_t)profileEnum);
  }

  int8_t txPower;
  if (pkt_tlv_get_i8(buf, TLV_TX_POWER, &txPower))
  {
    doc["power"] = txPower;
  }

  uint16_t sleepInterval;
  if (pkt_tlv_get_u16(buf, TLV_SLEEP_INTERVAL, &sleepInterval))
  {
    doc["sleep"] = sleepInterval;
  }

  uint32_t cmdMsgId;
  if (pkt_tlv_get_u32(buf, TLV_CMD_MSG_ID, &cmdMsgId))
  {
    doc["msg_id"] = cmdMsgId;
  }

  Serial.printf("[RX] Binary mode ACK from %s: profile=%s\n",
                devName, doc["profile"].as<const char *>());

  logMessage(doc, "lora");
  updateNodeState(doc);
}

// Handle binary status response (TX->RX status query response)
static void handleBinaryStatusResp(const uint8_t *buf, uint8_t pkt_len, int16_t rssi, float snr)
{
  uint16_t devId = pkt_device_id(buf);
  const char *devName = getDeviceName(devId);

  JsonDocument doc;
  doc["id"] = devName;
  doc["device"] = devName;
  doc["status"] = "ok";
  doc["received_at"] = millis();

  uint8_t profileEnum;
  if (pkt_tlv_get_u8(buf, TLV_PROFILE, &profileEnum))
  {
    doc["mode"] = profileToName((bp_profile_t)profileEnum);
  }

  int8_t txPower;
  if (pkt_tlv_get_i8(buf, TLV_TX_POWER, &txPower))
  {
    doc["power"] = txPower;
  }

  uint16_t sleepInterval;
  if (pkt_tlv_get_u16(buf, TLV_SLEEP_INTERVAL, &sleepInterval))
  {
    doc["sleep"] = sleepInterval;
  }

  uint8_t gpsWarm;
  if (pkt_tlv_get_u8(buf, TLV_GPS_WARM, &gpsWarm))
  {
    doc["gps_warm"] = (bool)gpsWarm;
  }

  uint8_t homeCycles;
  if (pkt_tlv_get_u8(buf, TLV_HOME_CYCLES, &homeCycles))
  {
    doc["home_cycles"] = homeCycles;
  }

  uint16_t logEntries, logSizeKB;
  if (pkt_tlv_get_log_info(buf, &logEntries, &logSizeKB))
  {
    char logStr[32];
    snprintf(logStr, sizeof(logStr), "%u entries, %u KB", logEntries, logSizeKB);
    doc["log"] = logStr;
  }

  uint32_t lostModeS;
  if (pkt_tlv_get_u32(buf, TLV_LOST_MODE_S, &lostModeS))
  {
    doc["lost_mode_s"] = lostModeS;
  }

  Serial.printf("[RX] Binary status response from %s: mode=%s\n",
                devName, doc["mode"].as<const char *>());

  logMessage(doc, "lora");
  updateNodeState(doc);
}

// Handle binary alert (TX->RX alert notification, e.g. lost mode timeout)
static void handleBinaryAlert(const uint8_t *buf, uint8_t pkt_len, int16_t rssi, float snr)
{
  uint16_t devId = pkt_device_id(buf);
  const char *devName = getDeviceName(devId);

  JsonDocument doc;
  doc["id"] = devName;
  doc["device"] = devName;
  doc["received_at"] = millis();

  uint8_t status = pkt_status(buf);

  if (status == STATUS_LOST_TIMEOUT)
  {
    doc["alert"] = "lost_mode_timeout";

    uint32_t durationS;
    if (pkt_tlv_get_u32(buf, TLV_DURATION_S, &durationS))
    {
      doc["duration_s"] = durationS;
    }

    uint8_t newModeEnum;
    if (pkt_tlv_get_u8(buf, TLV_NEW_MODE, &newModeEnum))
    {
      doc["new_mode"] = profileToName((bp_profile_t)newModeEnum);
    }

    Serial.printf("[RX] Binary alert from %s: lost_mode_timeout, reverted to %s\n",
                  devName, doc["new_mode"].as<const char *>());
  }
  else
  {
    doc["alert"] = "unknown";
    Serial.printf("[RX] Binary alert from %s: unknown status 0x%02X\n", devName, status);
  }

  logMessage(doc, "lora");
  updateNodeState(doc);
}
#endif // V3 ROLLOUT (binary handlers)

// ═════════════════════════════════════════════════════════════════════
// JSON HANDLER (V3 active path)
// ═════════════════════════════════════════════════════════════════════
static void handleLoRaPacketJSON(const String &incoming)
{
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, incoming);

  if (error)
  {
    Serial.println("[LORA] JSON parse error: " + String(error.c_str()));
    Serial.println("[LORA] Raw data: " + incoming);
    return;
  }

  if (!error && (doc["id"].is<String>() || doc["sender_name"].is<String>()))
  {
    // Map new field names to expected format
    if (doc["sender_name"].is<String>() && !doc["id"].is<String>())
    {
      doc["id"] = doc["sender_name"];
    }
    if (doc["latitude"].is<float>() && !doc["lat"].is<float>())
    {
      doc["lat"] = doc["latitude"];
    }
    if (doc["longitude"].is<float>() && !doc["lon"].is<float>())
    {
      doc["lon"] = doc["longitude"];
    }

    // Add receiver timestamp before storing and notifying
    doc["received_at"] = millis();

    // V3: receiver computes distance/bearing from home for every fix.
    // Collars only send raw lat/lon. Overwrites any dist_m/bearing the
    // collar may have included (older firmware may still send them).
    if (doc["lat"].is<float>() && doc["lon"].is<float>())
    {
      double catLat = doc["lat"].as<double>();
      double catLon = doc["lon"].as<double>();
      double distM = TinyGPSPlus::distanceBetween(catLat, catLon, g_homeLat, g_homeLon);
      double brng = TinyGPSPlus::courseTo(catLat, catLon, g_homeLat, g_homeLon);
      doc["dist_m"] = distM;
      doc["bearing"] = String((int)brng) + "-" + cardinalFromDegrees((uint16_t)brng);
    }

    // Normalize status to simplified 4-state system
    if (doc["status"].is<String>())
    {
      String originalStatus = doc["status"].as<String>();
      String lowerStatus = originalStatus;
      lowerStatus.toLowerCase();

      if (lowerStatus.indexOf("home") != -1)
      {
        doc["status"] = "Home";
      }
      else if (lowerStatus.indexOf("out") != -1 ||
               lowerStatus.indexOf("ok") != -1 ||
               lowerStatus.indexOf("normal") != -1 ||
               lowerStatus.indexOf("outanabout") != -1)
      {
        doc["status"] = "Out";
      }
      else if (lowerStatus.indexOf("offline") != -1)
      {
        doc["status"] = "Offline";
      }
      else if (lowerStatus.indexOf("error") != -1 ||
               lowerStatus.indexOf("invalid") != -1 ||
               lowerStatus.indexOf("no gps fix") != -1 ||
               lowerStatus.indexOf("no fix") != -1)
      {
        doc["status"] = "Error";
      }
      else
      {
        doc["status"] = "Out";
      }
    }
    else
    {
      doc["status"] = "Error";
    }

    String payload;
    serializeJson(doc, payload);
    catPayloads[doc["id"].as<String>()] = payload;

    serializeJsonPretty(doc, Serial);
    Serial.println();

    logMessage(doc, "lora");
    updateNodeState(doc);
    notifyPosition(doc);

    // V3: collar just transmitted — we have ~5s of post-TX RX window on the
    // collar side. If there's a queued command for it, push it now so it
    // lands inside that window instead of waiting on the 3s safety-net poll.
    String reporting = doc["id"].as<String>();
    if (reporting.length() > 0)
    {
      transmitCommandForDevice(reporting);
    }
  }
  else
  {
    Serial.println("[LORA] Invalid JSON or missing ID");
  }
}

// ═════════════════════════════════════════════════════════════════════
// DUAL-MODE LORA PACKET HANDLER
// ═════════════════════════════════════════════════════════════════════

void handleLoRaPacket()
{
  if (!packetReceived)
    return;

  packetReceived = false;

  // Read raw bytes from LoRa
  uint8_t rxBuf[256];
  size_t rxLen = 0;
  int state = lora.readData(rxBuf, sizeof(rxBuf));

  if (state == RADIOLIB_ERR_NONE)
  {
    rxLen = lora.getPacketLength();
    int16_t rssi = lora.getRSSI();
    float snr = lora.getSNR();

    // Flash LoRa LED on message receipt
    LED_flicker();

    if (rxLen == 0)
    {
      Serial.println("[LORA] Empty packet received");
      lora.startReceive();
      return;
    }

    // Detect protocol: first byte '{' (0x7B) = JSON, 0x01 = binary v1
    if (rxBuf[0] == '{')
    {
      // Legacy JSON packet
      rxBuf[rxLen] = '\0'; // Null-terminate for string parsing
      String incoming((char *)rxBuf);
      Serial.println("[LORA] JSON packet received: " + incoming);
      handleLoRaPacketJSON(incoming);
    }
#if 0 // V3 ROLLOUT: binary TLV inbound path disabled. Parked on wip/binary-migration branch.
    else if (rxBuf[0] == BP_PROTOCOL_VERSION)
    {
      // Binary protocol packet
      Serial.printf("[LORA] Binary packet received (%d bytes, RSSI=%d, SNR=%.1f)\n",
                    rxLen, rssi, snr);
      pkt_print_hex(rxBuf, rxLen);

      // Validate CRC
      if (!pkt_validate_crc(rxBuf, rxLen))
      {
        Serial.println("[LORA] Binary CRC validation failed - dropping packet");
        lora.startReceive();
        return;
      }

      // Dispatch by packet type
      uint16_t ptype = pkt_pkt_type(rxBuf);

      switch (ptype)
      {
      case PKT_TELEMETRY:
        handleBinaryTelemetry(rxBuf, rxLen, rssi, snr);
        break;
      case PKT_MODE_ACK:
        handleBinaryModeAck(rxBuf, rxLen, rssi, snr);
        break;
      case PKT_STATUS_RESP:
        handleBinaryStatusResp(rxBuf, rxLen, rssi, snr);
        break;
      case PKT_ALERT:
        handleBinaryAlert(rxBuf, rxLen, rssi, snr);
        break;
      default:
        Serial.printf("[LORA] Unknown binary packet type: 0x%04X\n", ptype);
        break;
      }
    }
#endif // V3 ROLLOUT
    else
    {
      Serial.printf("[LORA] Unknown/non-JSON packet (first byte: 0x%02X, %d bytes)\n",
                    rxBuf[0], rxLen);
    }
  }
  else if (state == RADIOLIB_ERR_PACKET_TOO_LONG)
  {
    Serial.println("[LORA] Error: Packet too long");
  }
  else if (state == RADIOLIB_ERR_CRC_MISMATCH)
  {
    Serial.println("[LORA] Error: CRC mismatch");
  }
  else if (state == RADIOLIB_ERR_INVALID_FREQUENCY)
  {
    Serial.println("[LORA] Error: Invalid frequency");
  }
  else
  {
    Serial.printf("[LORA] Unknown error: %d\n", state);
  }

  // Ensure LoRa module is reinitialized to receive the next packet
  lora.startReceive();
}

void setupGPS()
{
  gpsSerial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  // Initialize device location with HOME coordinates
  deviceLocation["id"] = "MyDevice";
  deviceLocation["lat"] = g_homeLat;
  deviceLocation["lon"] = g_homeLon;
  deviceLocation["status"] = "Starting up";
}

void handleDeviceOwnGPS()
{
  unsigned long startTime = millis();
  bool gpsDataProcessed = false; // Flag to track if any GPS data was processed in this call

  while (gpsSerial1.available() > 0 && (millis() - startTime) < 100) // Timeout after 100ms
  {
    if (gps.encode(gpsSerial1.read()))
    {
      gpsDataProcessed = true;                                 // Mark that we processed some GPS data
      if (gps.location.isValid() && gps.location.age() < 3000) // Check if data is fresh
      {
        deviceLocation["lat"] = gps.location.lat();
        deviceLocation["lon"] = gps.location.lng();
        deviceLocation["status"] = "Home"; // Use standard status: Home when GPS has fix
        deviceLocation["satellites"] = gps.satellites.value();
        deviceLocation["hdop"] = gps.hdop.value();
        deviceLocation["received_at"] = millis(); // Add receiver timestamp here too

        // Add GPS time information to the JSON document
        if (gps.time.isValid())
        {
          char timeStr[15]; // Buffer for formatted time string (HH:MM:SS.CC)
          sprintf(timeStr, "%02d:%02d:%02d.%02d",
                  gps.time.hour(),
                  gps.time.minute(),
                  gps.time.second(),
                  gps.time.centisecond());
          deviceLocation["gps_time"] = timeStr;
        }
        else
        {
          deviceLocation["gps_time"] = "INVALID";
        }
      }
      else
      {
        // Keep last known good location but update status
        deviceLocation["status"] = "Error";       // Use standard status: Error when no GPS fix
        deviceLocation["received_at"] = millis(); // Update timestamp even if no fix
      }
      // We break after the first valid sentence processing to avoid multiple updates from one burst
      break;
    }
  }

  // Send update every 10 seconds regardless of GPS data availability
  if (millis() - lastDeviceGPSUpdateTime >= DEVICE_GPS_UPDATE_INTERVAL)
  {
    // Update timestamp
    deviceLocation["received_at"] = millis();

    Serial.println("[GPS] Sending MyDevice update");

    // Log MyDevice GPS update
    logMessage(deviceLocation, "mydevice");

    notifyPosition(deviceLocation);     // Send as regular marker position
    lastDeviceGPSUpdateTime = millis(); // Reset timer after sending update
  }
}

void handleRoot()
{
  File file = LittleFS.open("/index.html", "r");
  if (!file)
  {
    server.send(500, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleData()
{
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (auto &pair : catPayloads)
  {
    JsonDocument singleDoc;
    DeserializationError error = deserializeJson(singleDoc, pair.second);
    if (!error)
      array.add(singleDoc);
  }
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void checkWiFiConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (isWiFiConnected)
    {
      Serial.println("[WIFI] Connection lost");
      isWiFiConnected = false;

      // Log WiFi disconnect event
      JsonDocument eventDoc;
      eventDoc["event"] = "wifi_disconnected";
      eventDoc["description"] = "WiFi connection lost";
      logMessage(eventDoc, "event");
    }
    Serial.print("[WIFI] Reconnecting...");
    WiFi.reconnect();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10)
    {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
      isWiFiConnected = true;
      Serial.println("\n[WIFI] Reconnected!");
      Serial.println(WiFi.localIP());

      // Log WiFi reconnect event
      JsonDocument eventDoc;
      eventDoc["event"] = "wifi_reconnected";
      eventDoc["description"] = "WiFi connection restored";
      logMessage(eventDoc, "event");
    }
  }
}

void setupBLE()
{
  BLEDevice::init(BLE_DEVICE_NAME);
  pAdvertising = BLEDevice::getAdvertising();

  // Configure minimal, non-connectable advertising payload
  BLEAdvertisementData advData;
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advData.setName("HOME"); // keep payload small (<31B)
  pAdvertising->setAdvertisementData(advData);

  // Non-scannable, non-connectable advertisement to reduce controller load
  pAdvertising->setScanResponse(false);
  pAdvertising->setAdvertisementType(ADV_TYPE_NONCONN_IND);

  // Set a conservative advertising interval (~1.0s) to minimize HCI traffic
  // Units are 0.625ms; 0x0640 = 1600 * 0.625ms = 1000ms
  pAdvertising->setMinInterval(0x0640);
  pAdvertising->setMaxInterval(0x0640);

  BLEDevice::startAdvertising();
  lastBLEAdvertTime = millis();
  Serial.println("[BLE] Advertising started (non-connectable) with name: HOME");
}

void enableBLE()
{
  if (!bleEnabled)
  {
    bleEnabled = true;
    // Always log the request
    Serial.println("[BLE] ✅ Beacon enabled (requested)");
    if (pAdvertising)
    {
      BLEDevice::startAdvertising();
      lastBLEAdvertTime = millis();
    }
    // Notify all clients of new state
    sendBleStateWS();
  }
}

void disableBLE()
{
  if (bleEnabled)
  {
    bleEnabled = false;
    // Always log the request
    Serial.println("[BLE] ❌ Beacon disabled (requested)");
    if (pAdvertising)
    {
      BLEDevice::stopAdvertising();
    }
    // Notify all clients of new state
    sendBleStateWS();
  }
}

// Broadcast or unicast current BLE state over WebSocket
void sendBleStateWS(uint8_t clientId)
{
  // Build tiny JSON manually to avoid any serialization pitfalls
  String out = String("{\"type\":\"ble_state\",\"on\":") + (bleEnabled ? "true" : "false") + "}";
  if (clientId == 255)
  {
    Serial.printf("[WS] Sending ble_state to ALL: %s\n", out.c_str());
    webSocket.broadcastTXT(out);
  }
  else
  {
    Serial.printf("[WS] Sending ble_state to client %u: %s\n", clientId, out.c_str());
    webSocket.sendTXT(clientId, out);
  }
}

void handleWebSocketMessage(uint8_t num, uint8_t *payload, size_t length)
{
  // Log payload as string for debug (safe copy)
  String message;
  message.reserve(length + 1);
  for (size_t i = 0; i < length; i++)
    message += (char)payload[i];
  Serial.printf("[WS] Client %u sent: %s\n", num, message.c_str());

  // Parse JSON command directly from payload buffer
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (!error && doc["type"].is<String>())
  {
    String commandType = doc["type"].as<String>();

    if (commandType == "ble_set")
    {
      bool turnOn = false;
      if (doc["on"].is<bool>())
      {
        turnOn = doc["on"].as<bool>();
      }
      Serial.printf("[WS] BLE set from client %u: %s\n", num, turnOn ? "ON" : "OFF");
      if (turnOn)
      {
        enableBLE();
      }
      else
      {
        disableBLE();
      }

      // Send confirmation back to client
      sendBleStateWS(num);
      return;
    }

    if (commandType == "ble_get")
    {
      Serial.printf("[WS] BLE get requested by client %u\n", num);
      // Reply to requester with current state
      sendBleStateWS(num);
      return;
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("[BOOT] Starting setup...");
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // Turn off the LED
  pinMode(LORA_LED, OUTPUT);      // Initialize LoRa LED pin
  digitalWrite(LORA_LED, LOW);    // Turn off LoRa LED
  setupBLE();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  { // Add timeout
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    isWiFiConnected = true;
    Serial.println("\n[WIFI] Connected!");
    Serial.printf("[WIFI] IP address: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("\n[WIFI] ❌ Failed to connect!");
    ESP.restart(); // Restart if WiFi fails
  }

  // Initialize mDNS
  if (MDNS.begin("cattracker"))
  {
    Serial.println("[mDNS] mDNS responder started. Access via http://cattracker.local");
  }
  else
  {
    Serial.println("[mDNS] ❌ Failed to start mDNS responder");
  }

  // ───────────── ArduinoOTA (V3) ─────────────
  // Push firmware over WiFi from PlatformIO with:
  //   pio run -t upload --upload-port cattracker.local
  // (platformio.ini sets upload_protocol = espota.) No password by default;
  // if you want one, call ArduinoOTA.setPassword("...") before begin().
  ArduinoOTA.setHostname("cattracker");
  ArduinoOTA.onStart([]() {
    const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("[OTA] Start updating %s\n", type);
    // If updating SPIFFS/LittleFS, unmount it first
    if (ArduinoOTA.getCommand() == U_SPIFFS) {
      LittleFS.end();
    }
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End — rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR:    Serial.println("Auth failed"); break;
      case OTA_BEGIN_ERROR:   Serial.println("Begin failed"); break;
      case OTA_CONNECT_ERROR: Serial.println("Connect failed"); break;
      case OTA_RECEIVE_ERROR: Serial.println("Receive failed"); break;
      case OTA_END_ERROR:     Serial.println("End failed"); break;
      default:                Serial.println("Unknown"); break;
    }
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] ArduinoOTA ready — upload to cattracker.local");

  // Mount filesystem with better error reporting
  if (!LittleFS.begin(true))
  { // Add format on fail
    Serial.println("[FS] ❌ LittleFS mount failed");
    delay(1000);
    ESP.restart();
  }

  // Add filesystem capacity information
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  float totalMB = totalBytes / (1024.0 * 1024.0);
  float usedMB = usedBytes / (1024.0 * 1024.0);
  float freeMB = (totalBytes - usedBytes) / (1024.0 * 1024.0);

  Serial.printf("[FS] LittleFS Capacity: %.2fMB used out of %.2fMB total, %.2fMB free\n",
                usedMB, totalMB, freeMB);
  delay(1000); // Give time for the filesystem to settle and view the setup messages

  Serial.println("[FS] Listing LittleFS root directory:");
  Serial.println("[FS] Files in root directory:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file)
  {
    Serial.println(file.name());
    file = root.openNextFile(); // Get the next file
  }
  delay(3000); // Give time for the filesystem to settle and viewthe setup messages

  // Load persisted home location (defaults applied if file missing/invalid)
  loadHomeLocation();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/messages.json", HTTP_GET, handleMessagesExport);
  server.on("/clear-log", HTTP_POST, handleClearLog);
  server.on("/node-states", HTTP_GET, handleNodeStates);    // Get node operating modes
  server.on("/send-command", HTTP_POST, handleSendCommand); // Send command to node
  server.on("/home", HTTP_GET, handleGetHome);              // Get current home lat/lon
  server.on("/home", HTTP_POST, handleSetHome);             // Set & persist home lat/lon
  server.serveStatic("/", LittleFS, "/");
  server.begin();
  Serial.println("[INFO] HTTP server started");
  Serial.printf("[INFO] Open http://cattracker.local or http://%s in your browser\n", WiFi.localIP().toString().c_str());

  webSocket.begin();
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t length)
                    {
    if (type == WStype_CONNECTED)
    {
      Serial.printf("[WS] Client %u connected\n", num);
      connectedClients++;
      // Immediately send current BLE status to this client
      sendBleStateWS(num);
    }
    else if (type == WStype_TEXT)
    {
      handleWebSocketMessage(num, payload, length);
    }
    else if (type == WStype_DISCONNECTED)
    {
      Serial.printf("[WS] Client %u disconnected\n", num);
    }
    else if (type == WStype_ERROR)
    {
      Serial.printf("[WS] Client %u error: %s\n", num, payload);
    } });
  LoRaSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  int state = lora.begin(LORA_FREQ_MHZ);
  lora.setOutputPower(22); // RX base station: always max power (mains-powered)
  lora.setSpreadingFactor(LORA_SF);
  lora.setBandwidth(LORA_BW_KHZ);
  lora.setCodingRate(LORA_CR);
  lora.setPreambleLength(LORA_PREAMBLE);
  lora.setCRC(LORA_USE_CRC);
  lora.setSyncWord(LORA_SYNC_WORD);
  lora.setDio1Action(onReceive);
  lora.startReceive(); // Start receiving packets
  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println("[INFO] LoRa initialized successfully.");
  }
  else
  {
    Serial.printf("[ERROR] LoRa init failed: %d\n", state);
    while (true)
      ;
  }

  setupGPS();

  // Initialize message logging system
  initMessageLog();

  // Flash builtin LED 5 times to indicate setup complete
  Serial.println("[BOOT] Setup complete - signaling ready");
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
  }
}

void loop()
{
  checkWiFiConnection();
  if (!isWiFiConnected)
  {
    Serial.println("[WIFI] ⚠️ No connection, waiting...");
    delay(1000);
    return;
  }

  // Handle HTTP server and WebSocket events
  server.handleClient();
  webSocket.loop();
  ArduinoOTA.handle(); // V3: service incoming OTA firmware uploads

  // Check if the serial port is open
  if (Serial && !serialPreviouslyOpened)
  {
    // Serial port has been reopened
    serialPreviouslyOpened = true;

    // Reprint startup information
    Serial.print("[WIFI] IP address: ");
    Serial.println(WiFi.localIP());
  }
  else if (!Serial && serialPreviouslyOpened)
  {
    // Serial port has been closed
    serialPreviouslyOpened = false;
  }

  // Handle GPS data continuously, the function itself will limit notification frequency
  handleDeviceOwnGPS();

  // Handle LoRa packets
  handleLoRaPacket();

  // Process command queue for LoRa transmission
  processCommandQueue();

  // Periodic flush of message log to LittleFS
  if (millis() - lastLogFlushTime >= LOG_FLUSH_INTERVAL)
  {
    flushMessageLog();
  }

  // No periodic re-advertising; BLE continues advertising until explicitly stopped/started
}

void notifyPosition(const JsonDocument &doc)
{
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.broadcastTXT(jsonString); // Broadcast the JSON to all WebSocket clients
  Serial.println("[WS] Position updated: " + jsonString);
}

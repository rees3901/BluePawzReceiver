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
#include <WiFi.h>
#include <WebServer.h>        // Include the WebServer library for HTTP server
#include <WebSocketsServer.h> // Include the WebSockets library for WebSocket server
#include <LittleFS.h>
#include <map> // Include the map library
#include <TinyGPS++.h>
#include <ESPmDNS.h> // Add mDNS library
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

SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, LoRaSPI);

volatile bool packetReceived = false;

// ───────────── LED Blink Timer Config ─────────────
bool ledState = false;
unsigned long lastToggle = 0;
const unsigned long toggleInterval = 4000;

// ───────────── GPS Configuration ─────────────
#define GPS_RX D0
#define GPS_TX D1
#define GPS_BAUD 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial1(1);

// Add initial location JSON
JsonDocument deviceLocation;
const float HOME_LAT = 51.87378215701798;
const float HOME_LON = -2.239428653198173;

// Add a flag to track the serial connection state
bool serialPreviouslyOpened = false;

// Add WiFi connection status
bool isWiFiConnected = false;

// Track WebSocket clients
uint8_t connectedClients = 0;

// Add timer for device GPS updates
unsigned long lastDeviceGPSUpdateTime = 0;
const unsigned long DEVICE_GPS_UPDATE_INTERVAL = 60000; // 1 minute in milliseconds

// ───────────── BLE Beacon Config ─────────────
#define BLE_DEVICE_NAME "CAT_TRACKER_HQ"
BLEAdvertising *pAdvertising = nullptr;
unsigned long lastBLEAdvertTime = 0;
const unsigned long BLE_ADVERT_INTERVAL = 3000; // 5 seconds
bool bleEnabled = true;                         // BLE beacon control flag

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
void setup();
void loop();

// Improve WebSocket notification with connection tracking

void onReceive()
{
  packetReceived = true;
  Serial.println("Receiving LoRa packet.."); // Debug log
}

void handleLoRaPacket()
{
  if (!packetReceived)
    return;

  packetReceived = false; // Reset the packetReceived flag to prepare for the next packet
  String incoming;
  int state = lora.readData(incoming);

  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println("[LORA] Packet received: " + incoming);
    if (incoming.length() > 0)
    {
      JsonDocument doc; // Increased buffer size for new format
      DeserializationError error = deserializeJson(doc, incoming);

      if (error)
      {
        Serial.println("[LORA] ❌ JSON parse error: " + String(error.c_str()));
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
        doc["received_at"] = millis(); // Add receiver's millis() timestamp

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
            // Default for any other status not matching above
            doc["status"] = "Out";
          }
        }
        else
        {
          // If status field is missing, default to "Error"
          doc["status"] = "Error";
        }

        String payload;              // Declare a String to hold the serialized JSON
        serializeJson(doc, payload); // Serialize the JSON into the String

        catPayloads[doc["id"].as<String>()] = payload; // Store the payload in the map

        serializeJsonPretty(doc, Serial);
        Serial.println();

        notifyPosition(doc); // Send the doc *with* the added timestamp
      }
      else
      {
        Serial.println("[LORA] ❌ Invalid JSON or missing ID");
      }
    }
    else
    {
      Serial.println("[LORA] ❌ Empty packet received");
    }
  }
  else if (state == RADIOLIB_ERR_PACKET_TOO_LONG)
  {
    Serial.println("[LORA] ⚠️ Error: Packet too long");
  }
  else if (state == RADIOLIB_ERR_CRC_MISMATCH)
  {
    Serial.println("[LORA] ⚠️ Error: CRC mismatch");
  }
  else if (state == RADIOLIB_ERR_INVALID_FREQUENCY)
  {
    Serial.println("[LORA] ⚠️ Error: Invalid frequency");
  }
  else
  {
    Serial.printf("[LORA] ⚠️ Unknown error: %d\n", state);
  }

  // Ensure LoRa module is reinitialized to receive the next packet
  lora.startReceive();
}

void setupGPS()
{
  gpsSerial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  // Initialize device location with HOME coordinates
  deviceLocation["id"] = "MyDevice";
  deviceLocation["lat"] = HOME_LAT;
  deviceLocation["lon"] = HOME_LON;
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
        deviceLocation["status"] = "GPS Fix";
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
        deviceLocation["status"] = "No GPS Fix";
        deviceLocation["received_at"] = millis(); // Update timestamp even if no fix
      }
      // We break after the first valid sentence processing to avoid multiple updates from one burst
      break;
    }
  }

  // Only send update if data was processed and the interval has passed
  if (gpsDataProcessed && (millis() - lastDeviceGPSUpdateTime >= DEVICE_GPS_UPDATE_INTERVAL))
  {
    notifyPosition(deviceLocation);
    lastDeviceGPSUpdateTime = millis(); // Reset timer after sending update
  }
  // Also send an update immediately if the status changes to "No GPS Fix" after having a fix
  else if (gpsDataProcessed && deviceLocation["status"] == "No GPS Fix" &&
           (millis() - lastDeviceGPSUpdateTime < DEVICE_GPS_UPDATE_INTERVAL) && // Ensure we don't double-send if interval also passed
           (millis() - lastDeviceGPSUpdateTime > 1000))                         // Add a small delay to prevent spamming "No GPS Fix"
  {
    // Check if the previous status was different (optional, needs state tracking)
    // For simplicity, we just send if status is "No GPS Fix" and interval hasn't passed
    // This ensures the UI reflects the loss of fix promptly.
    notifyPosition(deviceLocation);
    // We don't reset lastDeviceGPSUpdateTime here, so the next *timed* update still happens on schedule
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
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
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

  int state = lora.begin(915.0);
  lora.setOutputPower(22);
  lora.setSpreadingFactor(8);
  lora.setBandwidth(250.0);
  lora.setCodingRate(5);
  lora.setPreambleLength(8);
  lora.setCRC(true);
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

  // No periodic re-advertising; BLE continues advertising until explicitly stopped/started
}

void notifyPosition(const JsonDocument &doc)
{
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.broadcastTXT(jsonString); // Broadcast the JSON to all WebSocket clients
  Serial.println("[WS] Position updated: " + jsonString);
}

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
DynamicJsonDocument deviceLocation(250);
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

// Function declarations
void notifyClients();
void notifyPosition(const DynamicJsonDocument &doc);
void handleLoRaPacket();
void onReceive();
void setupGPS();
void handleDeviceOwnGPS();
void handleRoot();
void handleData();
void checkWiFiConnection();
void setupBLE();
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
    // Serial.println("[LORA] Packet received: from" + incoming);
    if (incoming.length() > 0)
    {
      DynamicJsonDocument doc(250);
      DeserializationError error = deserializeJson(doc, incoming);

      if (!error && doc.containsKey("id"))
      {
        // Add receiver timestamp before storing and notifying
        doc["received_at"] = millis(); // Add receiver's millis() timestamp

        // Normalize status
        if (doc.containsKey("status"))
        {
          String originalStatus = doc["status"].as<String>();
          String lowerStatus = originalStatus;
          lowerStatus.toLowerCase();

          if (lowerStatus.indexOf("home") != -1)
          {
            doc["status"] = "Home";
          }
          else if (lowerStatus.indexOf("ok") != -1 ||
                   lowerStatus.indexOf("normal") != -1)
          {
            doc["status"] = "Outanabout";
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
            doc["status"] = "Error";
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
  DynamicJsonDocument doc(250);
  JsonArray array = doc.to<JsonArray>();
  for (auto &pair : catPayloads)
  {
    DynamicJsonDocument singleDoc(250);
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
  BLEServer *pServer = BLEDevice::createServer();
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  lastBLEAdvertTime = millis();
  Serial.println("[BLE] Advertising started as beacon: " BLE_DEVICE_NAME);
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
    }
    else if (type == WStype_TEXT)
    {
      String msg = String((char *)payload);
      Serial.printf("[WS] Client %u sent: %s\n", num, msg.c_str());
    }
    else if (type == WStype_DISCONNECTED)
    {
      Serial.printf("[WS] Client %u disconnected\n", num);
    }
    else if (type == WStype_ERROR)
    {
      Serial.printf("[WS] Client %u error: %s\n", num, payload);
    } }); // <-- Add this closing parenthesis and semicolon {
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

  // ───── BLE beacon non-blocking refresh every 5 seconds ─────
  if (millis() - lastBLEAdvertTime >= BLE_ADVERT_INTERVAL)
  {
    if (pAdvertising)
    {
      BLEDevice::startAdvertising();
      // Serial.println("[BLE] Beacon re-advertised");
    }
    lastBLEAdvertTime = millis();
  }
}

void notifyPosition(const DynamicJsonDocument &doc)
{
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.broadcastTXT(jsonString); // Broadcast the JSON to all WebSocket clients
  Serial.println("[WS] Position updated: " + jsonString);
}

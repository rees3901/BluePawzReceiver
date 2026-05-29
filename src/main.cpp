/*
  ╔═══════════════════════════════════════════════════════════════════╗
  ║                                                                   ║
  ║  BLUEPAWZ RECEIVER  —  V3 (JSON protocol, Heltec V2 hardware)     ║
  ║                                                                   ║
  ║  Mains-powered base station that listens for collar telemetry     ║
  ║  over LoRa, serves a Leaflet.js map over Wi-Fi, and beacons       ║
  ║  a short-range BLE "Home" identifier so collars can detect        ║
  ║  when their cat is back indoors.                                  ║
  ║                                                                   ║
  ║  ─────────────────────────────────────────────────────────────    ║
  ║  This is a SINGLE-FILE Arduino sketch. Conceptually it is six     ║
  ║  loosely-coupled subsystems, each marked with a banner below:     ║
  ║                                                                   ║
  ║    1. Hardware bring-up        Vext rail, ST7735 TFT, LoRa SPI    ║
  ║    2. LoRa RX                  packet dispatch, JSON parsing,     ║
  ║                                  haversine distance/bearing       ║
  ║    3. LoRa TX (command queue)  opportunistic + safety-net send    ║
  ║                                  to collars during their post-TX  ║
  ║                                  RX window (Class-A LoRaWAN)      ║
  ║    4. HTTP + WebSocket server  the web UI lives in data/, this    ║
  ║                                  file just serves and pushes      ║
  ║    5. BLE beacon               -12 dBm, name "Home", indoor-only  ║
  ║                                  reach by design                  ║
  ║    6. ArduinoOTA               wireless firmware push from PIO    ║
  ║                                                                   ║
  ║  ─────────────────────────────────────────────────────────────    ║
  ║  EXECUTION MODEL                                                  ║
  ║                                                                   ║
  ║  Single-core super-loop (NOT FreeRTOS — that's the transmitter).  ║
  ║  Everything runs from loop() in order, fast enough that the       ║
  ║  ~1 Hz TFT refresh / ~3 s LoRa command interval / WebSocket       ║
  ║  servicing all comfortably keep up. LoRa RX is interrupt-driven   ║
  ║  via DIO1 → setRxFlag → packetReceived; the heavy lifting         ║
  ║  happens in handleLoRaPacket() from loop().                       ║
  ║                                                                   ║
  ║  ─────────────────────────────────────────────────────────────    ║
  ║  PERSISTENCE                                                      ║
  ║                                                                   ║
  ║    /home_location.json    LittleFS — dynamic home {lat,lon}       ║
  ║    /messages.json         LittleFS — circular log of last 500     ║
  ║                                       inbound + event messages    ║
  ║    nodeStates             in-memory std::map (lost on reboot)     ║
  ║                                                                   ║
  ║  WiFi creds live in include/secrets.h (NOT committed). Create     ║
  ║  it as: #define WIFI_SSID "..." / #define WIFI_PASSWORD "..."     ║
  ║                                                                   ║
  ║  ─────────────────────────────────────────────────────────────    ║
  ║  SEE ALSO                                                         ║
  ║                                                                   ║
  ║    README.md              quickstart, hardware, HTTP API          ║
  ║    ARCHITECTURE.md        end-to-end design (JSON wire format,    ║
  ║                              downlink timing, mode profiles,      ║
  ║                              binary-TLV history)                  ║
  ║    The transmitter repo: rees3901/BluePawzTransmitter             ║
  ║                                                                   ║
  ╚═══════════════════════════════════════════════════════════════════╝
*/

// ──────────────────────── LIBRARY INCLUDES ─────────────────────────
#include <Arduino.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <secrets.h> // Include your secrets.h file for WiFi credentials
#include <config.h>  // Shared configuration with TX nodes
#include "protocol.h"
#include "version.h" // BLUEPAWZ_VERSION — bump per semver in include/version.h
#include <WiFi.h>
#include <WebServer.h>        // Include the WebServer library for HTTP server
#include <WebSocketsServer.h> // Include the WebSockets library for WebSocket server
#include <LittleFS.h>
#include <map>    // Include the map library
#include <vector> // Include for message log buffer
#include <TinyGPS++.h>
#include <ESPmDNS.h> // Add mDNS library
#include <ArduinoOTA.h> // V3: wireless firmware push from PlatformIO (espota)
#include <Adafruit_GFX.h>     // V3: graphics primitives for the V2 TFT
#include <Adafruit_ST7735.h>  // V3: ST7735S driver for the Heltec V2 onboard display
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>

// ──────────────────────────── CONFIGURATION ───────────────────────────

WebServer server(80);
WebSocketsServer webSocket(81);
std::map<String, String> catPayloads;

// ─────────────────────────────────────────────────────────────────────
// Heltec Wireless Tracker V2 (HTIT-Tracker_V2.3) pin map
// Source: espressif/arduino-esp32 variants/heltec_wireless_tracker/pins_arduino.h
//         + Heltec_ESP32 HT_st7735 driver
//         + Wireless_Tracker_V2.3 schematic (user-verified)
// V2 differs from V1 in: ESP32-S3FN8 + SX1262 (default SPI, GPIO 8-14),
//   UC6580 GNSS @115200 (was NEO-6M @9600), built-in ST7735 colour TFT.
// ─────────────────────────────────────────────────────────────────────

// ───────────── LoRa SX1262 (default SPI bus) ─────────────
#define LORA_NSS 8
#define LORA_SCK 9
#define LORA_MOSI 10
#define LORA_MISO 11
#define LORA_RST 12
#define LORA_BUSY 13
#define LORA_DIO1 14

SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, LoRaSPI);

volatile bool packetReceived = false;

// ───────────── LED Blink Timer Config ─────────────
#define LORA_LED 18 // V2: onboard white LED on GPIO 18
bool ledState = false;
unsigned long lastToggle = 0;
const unsigned long toggleInterval = 4000;

// ───────────── GPS UC6580 ─────────────
// Verified against the Heltec HTIT-Tracker_V2.3 schematic (PDF in repo docs):
//   GNSS_TX (chip pin 19) → ESP32 GPIO 33 (the ESP32 RECEIVES on this pin)
//   GNSS_RX (chip pin 18) ← ESP32 GPIO 34 (the ESP32 TRANSMITS on this pin)
//   GNSS_RST (chip pin 17) ↔ ESP32 GPIO 35 (pulled high by R26 to Vext_3V3,
//      so the GPS comes out of reset when Vext goes HIGH; we toggle it
//      explicitly in setupGPS to force a clean cold start every boot)
//   PPS (chip pin 35) → ESP32 GPIO 36 (unused — would give 1pps sync if needed)
// UC6580 default baud is 115200 (V1's NEO-6M was 9600).
#define GPS_RX 33
#define GPS_TX 34
#define GPS_RST 35
#define GPS_PPS 36
#define GPS_BAUD 115200

// ───────────── Vext rail (powers GPS + TFT, ACTIVE LOW) ─────────────
#define VEXT_CTRL 3   // drive LOW = Vext ON

// ───────────── TFT ST7735S (built-in 160×80) ─────────────
#define TFT_MOSI 42
#define TFT_SCK  41
#define TFT_CS   38
#define TFT_DC   40
#define TFT_RST  39
#define TFT_BL   21   // backlight (HIGH = on)

TinyGPSPlus gps;
HardwareSerial gpsSerial1(1);

// V3: ST7735 TFT on Heltec V2. Using software-SPI constructor so we can put
// any GPIO on each role without colliding with the SX1262's HSPI bus. The TFT
// refresh rate (~1 Hz status panel) doesn't justify hardware SPI complexity.
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
static uint32_t tftLastRefresh = 0;
static uint32_t tftMsgCount = 0;             // total inbound LoRa packets seen since boot
static String   tftLastCatName = "";          // last cat that reported in
static int16_t  tftLastCatRssi = 0;

// Forward-declared so tftRefresh() (defined just below) can read the BLE
// state. The actual variable lives further down in the file alongside the
// other BLE-related globals.
extern bool bleEnabled;

// GPS diagnostic counters used by the TFT status indicator. Real definitions
// live just above setupGPS() further down the file.
extern uint32_t gpsBytesRx;
extern uint32_t gpsValidFixes;

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

// V3: cached so tftRefresh() (1 Hz) doesn't have to poke LittleFS every cycle.
// Set true once a successful load or save has happened; set false if a load
// finds no file. Without this cache, vfs_api.cpp logs an error every refresh
// when the file genuinely doesn't exist (very common on first boot).
bool g_homeLocationSaved = false;

// ───────────── Heltec V2 hardware bring-up ─────────────
// Vext is the external power rail on the Heltec V2. It feeds the UC6580 GNSS
// and the TFT backlight/logic. **On the Wireless Tracker V2 specifically it
// is ACTIVE HIGH** — drive the pin HIGH to enable the rail. This is the
// opposite of older Heltec boards (WiFi LoRa 32, etc.) which used active-LOW.
// Driving the wrong polarity = no GPS, no TFT, silent failure on cold boot
// with no diagnostic to lead you to the right answer. Verified against the
// vendor's reference sketch in the V2 docs.
static void heltecV2_enableVext()
{
  pinMode(VEXT_CTRL, OUTPUT);
  digitalWrite(VEXT_CTRL, HIGH); // Vext ON (active HIGH on V2!)
  delay(50);                     // give rails time to settle
}

// Initialise the ST7735 TFT and draw the boot splash.
static void tftBegin()
{
  // Backlight on (separate from Vext)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // V3 TFT init: the Heltec V2 ships a 160x80 ST7735 panel. Adafruit's lib
  // has THREE possible inits for similar panels — if the display looks
  // wrong, swap between them:
  //   INITR_MINI160x80        — original 160x80 panel (DEFAULT)
  //   INITR_MINI160x80_PLUGIN — newer panel batches with different init
  //   INITR_GREENTAB          — fallback if both above show offset/colour issues
  // Common visible symptoms:
  //   - blank display       → wrong init variant OR Vext polarity wrong
  //   - inverted colours    → invertDisplay(true) needed (this panel needs it)
  //   - shifted image       → panel uses non-standard X/Y offsets (1, 26)
  //   - garbled / random pixels → MOSI/SCK pin map wrong, or SPI mode mismatch
  tft.initR(INITR_MINI160x80);
  // Colour inversion: leave OFF on our specific Heltec V2 panel batch.
  // I initially set this to true based on a reference doc, which made the
  // panel "work" but with a WHITE background — the controller was flipping
  // every pixel, so BLACK fills rendered as WHITE and our named colours
  // (cyan, yellow, etc.) showed as their complements. With invertDisplay(false)
  // we get a proper BLACK background and the colour names match reality.
  // If a future hardware revision needs inversion, flip back to true and
  // expect to also re-pick all the colour constants to their complements.
  tft.invertDisplay(false);
  tft.setRotation(1);                  // landscape: 160 wide x 80 tall
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextSize(1);
  // Title bar = "BluePaws " + the current firmware version. See include/version.h.
  tft.print(F("BluePaws "));
  tft.print(F("v"));
  tft.print(BLUEPAWZ_VERSION);
  tft.setCursor(2, 14);
  tft.print(F("Booting..."));
}

// Periodically redraw the small status panel. Called from loop() — guards
// against too-frequent redraws to avoid CPU cost.
//
// FLICKER NOTE: an earlier version did `tft.fillScreen(ST77XX_BLACK)` and
// redrew everything from scratch. With ~16 ms between clear and the first
// glyph landing, you got a visible black flash every 1 s — annoying on a
// status panel that lives on your desk. This version writes each glyph
// with an explicit black BACKGROUND, so the new text overwrites the old
// in place. No clear, no flicker. Every value is padded to a fixed width
// (snprintf with "%-Ns") so a shorter new value (e.g. RSSI=-90 → -8) wipes
// out the trailing characters of the previous longer value.
static void tftRefresh()
{
  if (millis() - tftLastRefresh < 1000) return; // 1 Hz max
  tftLastRefresh = millis();

  char buf[32];
  tft.setTextSize(1);

  // Title bar
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(2, 2);
  snprintf(buf, sizeof(buf), "BluePaws v%-12s", BLUEPAWZ_VERSION);
  tft.print(buf);

  // WiFi status / IP
  tft.setCursor(2, 14);
  if (WiFi.status() == WL_CONNECTED)
  {
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    snprintf(buf, sizeof(buf), "%-20s", WiFi.localIP().toString().c_str());
  }
  else
  {
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    snprintf(buf, sizeof(buf), "%-20s", "WiFi: down");
  }
  tft.print(buf);

  // Packets seen since boot
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(2, 28);
  snprintf(buf, sizeof(buf), "Pkts: %-14u", tftMsgCount);
  tft.print(buf);

  // Last cat that reported in + its RSSI
  tft.setCursor(2, 42);
  if (tftLastCatName.length() > 0)
  {
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    snprintf(buf, sizeof(buf), "%s %ddBm        ", tftLastCatName.c_str(), tftLastCatRssi);
  }
  else
  {
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    snprintf(buf, sizeof(buf), "%-20s", "(no cats yet)");
  }
  tft.print(buf);

  // Home location: shown small in the corner so users can sanity-check.
  // Uses the cached g_homeLocationSaved flag instead of LittleFS.exists()
  // because exists() internally open()s the file, and vfs_api logs an
  // error on every failed open — would flood the console at 1 Hz on a
  // fresh install where the user hasn't set a home yet.
  tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
  tft.setCursor(2, 56);
  snprintf(buf, sizeof(buf), "Home set: %-10s", g_homeLocationSaved ? "yes" : "no");
  tft.print(buf);

  // BLE beacon state — on the SAME row as the GPS status (right-half) so we
  // can fit them both onto the panel.
  tft.setTextColor(bleEnabled ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);
  tft.setCursor(2, 68);
  snprintf(buf, sizeof(buf), "BLE: %-3s", bleEnabled ? "on" : "off");
  tft.print(buf);

  // GPS status line — full-width row at the very bottom of the panel.
  // This is the LAST thing drawn so it can't be hidden by anything else.
  // Uses big readable text + a full-row background colour so the state is
  // unmistakable from across the room.
  //
  //   RED   "GPS NO DATA"     no NMEA bytes ever       hardware fault
  //   AMBER "GPS ACQUIRING"   bytes but no lock yet    cold start in progress
  //   GREEN "GPS LOCKED Nsat" fresh fix                all good
  //
  static const uint16_t TFT_AMBER = 0xFC00;
  uint16_t gpsColour;
  const char *gpsText;
  char gpsBuf[24];
  if (gpsBytesRx == 0)
  {
    gpsColour = ST77XX_RED;
    gpsText = "GPS NO DATA";
  }
  else if (gpsValidFixes == 0)
  {
    gpsColour = TFT_AMBER;
    gpsText = "GPS ACQUIRING";
  }
  else if (gps.location.age() < 5000)
  {
    gpsColour = ST77XX_GREEN;
    snprintf(gpsBuf, sizeof(gpsBuf), "GPS LOCK %u sat", (unsigned)gps.satellites.value());
    gpsText = gpsBuf;
  }
  else
  {
    gpsColour = TFT_AMBER;
    gpsText = "GPS STALE";
  }
  // Wider GPS pill on the bottom-right. Avoids overlapping the BLE label.
  tft.fillRect(60, 68, 100, 11, gpsColour);
  tft.setTextColor(ST77XX_BLACK, gpsColour);
  tft.setCursor(63, 70);
  tft.print(gpsText);
}

static bool loadHomeLocation()
{
  if (!LittleFS.exists(HOME_LOCATION_FILE))
  {
    Serial.println("[HOME] No saved home location, using defaults");
    g_homeLocationSaved = false;
    return false;
  }
  File f = LittleFS.open(HOME_LOCATION_FILE, "r");
  if (!f)
  {
    Serial.println("[HOME] Failed to open home_location.json for read");
    g_homeLocationSaved = false;
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    Serial.printf("[HOME] Parse error: %s — falling back to defaults\n", err.c_str());
    g_homeLocationSaved = false;
    return false;
  }
  if (doc["lat"].is<float>() && doc["lon"].is<float>())
  {
    g_homeLat = doc["lat"].as<float>();
    g_homeLon = doc["lon"].as<float>();
    Serial.printf("[HOME] Loaded: lat=%.6f lon=%.6f\n", g_homeLat, g_homeLon);
    g_homeLocationSaved = true;
    return true;
  }
  Serial.println("[HOME] Missing lat/lon in file — using defaults");
  g_homeLocationSaved = false;
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
  g_homeLocationSaved = true;
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
  String deviceId;                // friendly name ("Podge"), can be renamed
  uint16_t deviceIdNum = 0;       // immutable numeric id from collar's DEVICE_ID_INT (0 = unknown)
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
void sendRenameCommand(uint16_t deviceIdNum, const String &newName);
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

// Add a message to the in-memory circular log.
//
// Two-tier logging strategy:
//   1) Every inbound LoRa packet (telemetry, ACK, event) gets pushed into
//      messageLogBuffer (capacity MAX_LOG_MESSAGES = 500). Oldest entries
//      are dropped when full.
//   2) Every LOG_FLUSH_INTERVAL ms (60 s) flushMessageLog() rewrites the
//      buffer to /messages.json on LittleFS.
//
// Rationale: flash wear matters. Writing on every packet would chew through
// LittleFS in weeks. 60 s flush is a sane trade-off between durability and
// wear; if power is yanked we lose at most one minute of recent messages.
// The serial monitor still prints everything in real time regardless.
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
// NODE STATE TRACKING & DOWNLINK COMMANDS
// ═════════════════════════════════════════════════════════════════════
//
// Two halves of the same concern: keep track of what every collar's
// current state is (for the UI), and queue + send commands back to
// individual collars.
//
// nodeStates is keyed by friendly name (String) for fast lookup by the
// UI. We also store the immutable numeric device_id inside each
// NodeState so renames can be detected: if a packet arrives whose
// device_id matches an existing entry but whose "id" (name) differs,
// the old entry is a rename ghost and gets dropped.
//
// commandQueue is a FIFO of pending downlink commands. Two paths
// consume it:
//   - transmitCommandForDevice() — called from handleLoRaPacketJSON
//     whenever a collar reports in. Bypasses the rate gate because
//     we KNOW the collar is in its post-TX RX window right now.
//   - processCommandQueue() — called from loop() as a safety-net
//     retry path. Rate-limited to COMMAND_TX_INTERVAL between sends.

// Update node state based on received message or ACK.
//
// Detects renames by comparing the incoming (name, device_id) against
// existing entries: a matching device_id under a different name means a
// rename happened, so the stale name is removed from the C&C list.
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

  // Capture numeric device_id up-front so we can detect renames.
  uint16_t incomingDevIdNum = 0;
  if (doc["device_id"].is<int>())
  {
    incomingDevIdNum = (uint16_t)doc["device_id"].as<int>();
  }

  // Rename housekeeping: if a different node entry already holds this
  // numeric device_id under a stale name (e.g. "Device-4" before rename),
  // drop it so the C&C UI doesn't show a ghost card.
  if (incomingDevIdNum != 0)
  {
    for (auto it = nodeStates.begin(); it != nodeStates.end(); )
    {
      if (it->second.deviceIdNum == incomingDevIdNum && it->first != deviceId)
      {
        Serial.printf("[RENAME] device_id=%u changed name '%s' -> '%s' — dropping stale entry\n",
                      incomingDevIdNum, it->first.c_str(), deviceId.c_str());
        it = nodeStates.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  // Get or create node state
  NodeState &state = nodeStates[deviceId];
  state.deviceId = deviceId;
  state.lastSeen = millis();
  if (incomingDevIdNum != 0) state.deviceIdNum = incomingDevIdNum;

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

// V3: rename a collar (set_name). Targets by numeric device_id because the
// current name may be unknown (e.g. a freshly-flashed collar reporting in as
// "Device-4"). The collar saves the new name to NVS and ACKs with the new id,
// so the UI sees the change on the very next inbound packet.
//
// Wire format:
//   {"cmd":"set_name","device_id":4,"name":"Podge","msg_id":N}
void sendRenameCommand(uint16_t deviceIdNum, const String &newName)
{
  // Validate name locally too — saves a round-trip if it's obviously bad.
  if (newName.length() == 0 || newName.length() > 15)
  {
    Serial.printf("[CMD] Rename rejected: name length %u not in 1..15\n", newName.length());
    return;
  }
  for (size_t i = 0; i < newName.length(); i++)
  {
    char c = newName[i];
    if ((unsigned char)c < 0x20 || c == ',' || c == '"' || c == '\\')
    {
      Serial.printf("[CMD] Rename rejected: name contains forbidden char 0x%02X\n",
                    (unsigned char)c);
      return;
    }
  }

  // Resolve a friendly current-name for the queue's targetDevice field, so
  // transmitCommandForDevice can route it on the next telemetry arrival.
  // (Falls back to the numeric form if we have no name yet.)
  String targetName = String("Device-") + String(deviceIdNum);
  const char *registryName = getDeviceName(deviceIdNum);
  if (registryName && strlen(registryName) > 0) targetName = registryName;

  LoRaCommand cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.targetDevice = targetName;
  cmd.targetDeviceId = deviceIdNum;
  cmd.timestamp = millis();
  cmd.messageId = nextMessageId++;

  JsonDocument doc;
  doc["cmd"] = "set_name";
  doc["device_id"] = deviceIdNum;
  doc["name"] = newName;
  doc["msg_id"] = cmd.messageId;
  size_t written = serializeJson(doc, cmd.buf, sizeof(cmd.buf));
  cmd.len = (uint8_t)written;

  commandQueue.push_back(cmd);

  Serial.printf("[CMD] Rename queued: device_id=%u -> '%s' (%u bytes JSON)\n",
                deviceIdNum, newName.c_str(), (unsigned)written);
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
    node["device_id"] = state.deviceIdNum;
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
  if (!server.hasArg("action"))
  {
    server.send(400, "text/plain", "Missing parameter: action");
    return;
  }
  String action = server.arg("action");

  // Most actions target by name. `rename` targets by numeric device_id because
  // the current name may be unknown (e.g. default "Device-4").
  if (action == "rename")
  {
    if (!server.hasArg("device_id") || !server.hasArg("name"))
    {
      server.send(400, "text/plain", "Rename requires device_id and name parameters");
      return;
    }
    long idNum = server.arg("device_id").toInt();
    if (idNum <= 0 || idNum > 0xFFFE)
    {
      server.send(400, "text/plain", "device_id out of range");
      return;
    }
    String newName = server.arg("name");
    sendRenameCommand((uint16_t)idNum, newName);
    server.send(200, "text/plain", "Rename command queued");
    return;
  }

  // All other actions take a device name
  if (!server.hasArg("device"))
  {
    server.send(400, "text/plain", "Missing parameter: device");
    return;
  }
  String deviceId = server.arg("device");

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

// HTTP handler: GET /version — return the receiver firmware version.
// The web UI fetches this on page load so the user can see at a glance
// which firmware their browser is talking to. Useful right after an OTA
// push to confirm the new image actually booted.
void handleGetVersion()
{
  JsonDocument doc;
  doc["version"] = BLUEPAWZ_VERSION;
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
    node["device_id"] = state.deviceIdNum;
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
      tftLastCatName = reporting;     // V3: surface on the V2 onboard TFT
      transmitCommandForDevice(reporting);
    }
  }
  else
  {
    Serial.println("[LORA] Invalid JSON or missing ID");
  }
}

// ═════════════════════════════════════════════════════════════════════
// LORA RX PACKET DISPATCH
// ═════════════════════════════════════════════════════════════════════
//
// Called from loop() once per pass. The packetReceived flag is set by
// onReceive() (ISR-attached to DIO1) when the SX1262 finishes decoding
// a packet. We snapshot the bytes, dispatch by protocol version, and
// reset the radio for the next packet.
//
// V3 dispatch policy:
//   - First byte '{' (0x7B) → JSON path, the only one we actually use
//   - First byte == BP_PROTOCOL_VERSION (0x01) → wrapped in #if 0, parked
//     on wip/binary-migration; the four handleBinary* functions are
//     similarly parked
//   - Anything else → logged and discarded
//
// All processing is synchronous on the main loop. lora.transmit() inside
// transmitCommandForDevice() blocks for ~200-300 ms while the command
// goes out; during that time the radio is in TX state and won't hear new
// inbound packets. With 5 collars on 5-minute cycles the odds of a
// collision are negligible.

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

      // V3: stash RSSI + bump counter for the TFT status panel before parsing.
      // The cat name is set below inside handleLoRaPacketJSON.
      tftMsgCount++;
      tftLastCatRssi = rssi;

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

// ───── GPS diagnostic counters (visible via serial + status panel) ─────
// These help distinguish "GPS isn't talking to me" (gpsBytesRx stays 0)
// from "GPS is talking but has no fix yet" (gpsBytesRx climbs, gpsValidFixes
// stays 0) from "all good" (both climb). Without them, a silent GPS UART
// looks identical to a GPS that's just busy acquiring satellites — both
// produce status="Starting up" with the placeholder coordinates.
uint32_t gpsBytesRx     = 0;  // total raw NMEA bytes received (extern in tftRefresh)
uint32_t gpsValidFixes  = 0;  // # of complete sentences with a valid fix
uint32_t gpsLastReportMs = 0;

// V3.0.4 NMEA debug: when true the receiver echoes each complete NMEA
// sentence to the serial monitor as it arrives. Lets you see exactly what
// the UC6580 is producing — talker IDs ($GP/$GN/$GL/$BD/$GA), fix quality
// field, satellite counts, the lot. Set to false to silence the (~700
// bytes/sec) firehose once the receiver is reliably getting fixes.
// Set true to echo every complete NMEA sentence to serial as it arrives.
// Useful for debugging "no fix" / antenna issues — see ~700 bytes/sec
// of NMEA chatter prefixed with [NMEA]. Default false because once the
// receiver is reliably getting fixes, the firehose just clutters the log.
#define GPS_NMEA_DEBUG false
static char    nmeaLineBuf[128];
static uint8_t nmeaLineLen = 0;

void setupGPS()
{
  // Make sure Vext is asserted (re-driving here in case anything earlier
  // in setup() momentarily clobbered the pin). Active HIGH on Heltec V2.
  pinMode(VEXT_CTRL, OUTPUT);
  digitalWrite(VEXT_CTRL, HIGH);

  // Reset the UC6580. R26 (10K) on the schematic pulls GNSS_RST high to
  // Vext_3V3 so the chip is normally out of reset; an explicit LOW→HIGH
  // pulse here forces a known-good cold start.
  pinMode(GPS_RST, OUTPUT);
  digitalWrite(GPS_RST, LOW);
  delay(100);
  digitalWrite(GPS_RST, HIGH);
  delay(500);     // 500 ms for chip to boot + start NMEA output

  // SINGLE begin() — V3.0.1 lesson: the previous auto-detect did
  //   gpsSerial1.end() / delay(20) / gpsSerial1.begin() multiple times.
  // The redundant end+begin cycle leaves the ESP32-S3 UART driver in a
  // state where the sniff inside setupGPS works fine but the loop()
  // handler that comes later reads zero bytes — driver wedged after the
  // re-init. Lesson: install the driver once and leave it alone.
  //
  // The UC6580's verified default baud is 115200 (last boot's auto-detect
  // confirmed: 1215 bytes of NMEA-like data in 1.5 s). Hardcoding it.
  gpsSerial1.setRxBufferSize(1024);
  gpsSerial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.printf("[GPS] UART1 up: rx=GPIO%d tx=GPIO%d baud=%d (UC6580)\n",
                GPS_RX, GPS_TX, GPS_BAUD);

  // One-shot sniff for confirmation only. We DON'T re-init the UART after
  // this; the begin() above is the one that loop() will keep using.
  uint32_t sniffStart = millis();
  uint32_t sniffBytes = 0;
  uint32_t printableBytes = 0;
  while (millis() - sniffStart < 1500)
  {
    while (gpsSerial1.available() > 0)
    {
      int c = gpsSerial1.read();
      sniffBytes++;
      if ((c >= 0x20 && c <= 0x7E) || c == '\r' || c == '\n') printableBytes++;
    }
    delay(10);
  }
  Serial.printf("[GPS] post-reset sniff: %u bytes, %u printable%s\n",
                sniffBytes, printableBytes,
                (sniffBytes > 20 && printableBytes * 100 / sniffBytes > 80)
                    ? " ✓ NMEA-like" : "");
  if (sniffBytes == 0)
  {
    Serial.println("[GPS] WARNING: 0 bytes in post-reset sniff window.");
    Serial.println("[GPS]   GPS may be unpowered, in reset, or the chip is dead.");
  }

  // Initialize device location with default home until the GPS produces
  // a real fix. Status field is what the UI keys on — "Starting up" means
  // we have no real data; flips to "Home" / "Error" once GPS parser runs.
  deviceLocation["id"]     = "MyDevice";
  deviceLocation["lat"]    = g_homeLat;
  deviceLocation["lon"]    = g_homeLon;
  deviceLocation["status"] = "Starting up";
}

// Pump bytes out of the GPS UART, feed them into TinyGPSPlus, update
// deviceLocation when a valid fix is parsed. Called from loop() once per
// pass — cheap (a handful of bytes per call typically). Tracks raw byte
// count and valid-fix count so we can diagnose "no signal" vs "no fix".
//
// The MyDevice WebSocket broadcast is gated by DEVICE_GPS_UPDATE_INTERVAL
// (10 s) so the map UI gets a steady once-per-10-sec heartbeat regardless
// of how fast NMEA is flowing in.
void handleDeviceOwnGPS()
{
  unsigned long startTime = millis();

  // Process up to ~100 ms worth of UART bytes per call. We don't drain the
  // whole queue every loop because TinyGPSPlus::encode() returns true after
  // each COMPLETE sentence, and we want to keep loop() responsive for the
  // HTTP server + WebSocket clients.
  while (gpsSerial1.available() > 0 && (millis() - startTime) < 100)
  {
    char c = gpsSerial1.read();
    if (gpsBytesRx == 0)
    {
      Serial.printf("[GPS] First byte received by loop(): 0x%02X ('%c')\n",
                    (unsigned)(uint8_t)c, (c >= 0x20 && c <= 0x7E) ? c : '?');
    }
    gpsBytesRx++;

#if GPS_NMEA_DEBUG
    // Accumulate bytes into a line buffer and print complete sentences
    // (terminated by \n) prefixed with [NMEA]. Lets you read raw output
    // straight off the serial monitor without splicing the byte stream.
    if (c == '\n')
    {
      if (nmeaLineLen > 0)
      {
        nmeaLineBuf[nmeaLineLen] = '\0';
        // Trim trailing \r if present so the print doesn't add a blank line
        if (nmeaLineLen > 0 && nmeaLineBuf[nmeaLineLen - 1] == '\r')
          nmeaLineBuf[nmeaLineLen - 1] = '\0';
        Serial.print("[NMEA] ");
        Serial.println(nmeaLineBuf);
      }
      nmeaLineLen = 0;
    }
    else if (nmeaLineLen < sizeof(nmeaLineBuf) - 1)
    {
      nmeaLineBuf[nmeaLineLen++] = c;
    }
    else
    {
      // Overflowed buffer (sentence longer than 127 chars — shouldn't
      // happen with valid NMEA but reset rather than crash). Drop the line.
      nmeaLineLen = 0;
    }
#endif

    if (gps.encode(c))
    {
      // A complete NMEA sentence was parsed. Check if location is fresh
      // (age < 3 s) and valid (the talker has actually acquired a fix —
      // GGA/RMC with sat lock, not just NMEA being emitted with empty fields).
      if (gps.location.isValid() && gps.location.age() < 3000)
      {
        gpsValidFixes++;
        deviceLocation["lat"]         = gps.location.lat();
        deviceLocation["lon"]         = gps.location.lng();
        deviceLocation["status"]      = "Home";   // UI status: green/home marker
        deviceLocation["satellites"]  = gps.satellites.value();
        deviceLocation["hdop"]        = gps.hdop.value();
        deviceLocation["received_at"] = millis();

        if (gps.time.isValid())
        {
          char timeStr[15];
          sprintf(timeStr, "%02d:%02d:%02d.%02d",
                  gps.time.hour(), gps.time.minute(),
                  gps.time.second(), gps.time.centisecond());
          deviceLocation["gps_time"] = timeStr;
        }
        else
        {
          deviceLocation["gps_time"] = "INVALID";
        }
      }
      else
      {
        // Sentence parsed but no valid lock. Keep the last known good
        // lat/lon (might have come from a previous fix or the placeholder
        // from setupGPS) and just flag the loss of fix to the UI.
        deviceLocation["status"]      = "Error";
        deviceLocation["received_at"] = millis();
      }
      // Break after one complete sentence — we'll get the next on the next
      // loop iteration. Stops a single burst monopolising the call.
      break;
    }
  }

  // Diagnostic heartbeat: every 5 s, dump the byte/fix counters to serial.
  // Frequent enough that the user always sees current state without having
  // to catch the boot-time logs. Decision tree from these numbers:
  //   rx_bytes = 0              → silent UART (Vext, reset pin, wiring)
  //   rx_bytes climbs, fixes=0  → GPS alive, acquiring sats (be patient)
  //   both climb                → healthy
  if (millis() - gpsLastReportMs > 5000)
  {
    gpsLastReportMs = millis();
    // Expanded diag: charsProcessed/sentencesWithFix/failedChecksum come
    // straight from TinyGPSPlus and tell us whether the parser is keeping
    // up. If charsProcessed climbs but sentencesWithFix stays 0, the lib
    // is parsing NMEA but the GGA/RMC sentences carry an empty fix —
    // i.e. the chip is talking but hasn't locked yet. failedChecksum
    // climbing fast = corrupted UART (baud, wiring, noise).
    Serial.printf("[GPS] diag: rx_bytes=%u valid_fixes=%u sats=%u hdop=%u age=%lu ms\n",
                  gpsBytesRx, gpsValidFixes,
                  (unsigned)gps.satellites.value(),
                  (unsigned)gps.hdop.value(),
                  (unsigned long)gps.location.age());
    Serial.printf("[GPS] parser: chars=%lu sentences_with_fix=%lu failed_csum=%lu passed_csum=%lu\n",
                  (unsigned long)gps.charsProcessed(),
                  (unsigned long)gps.sentencesWithFix(),
                  (unsigned long)gps.failedChecksum(),
                  (unsigned long)gps.passedChecksum());
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

// V3 BLE policy:
//  - The collar matches BEACON_NAME ("Home") case-sensitively against
//    dev.getName(). We MUST advertise the exact same string, hence "Home".
//  - We deliberately advertise at the lowest sensible TX power so the beacon
//    has a short physical reach — collars detecting it should genuinely be
//    inside the house, not on the pavement out front. The collar enforces
//    a stricter RSSI threshold on top, but cutting TX power keeps things
//    sane even if a collar's RSSI drift moves the threshold around.
//  - ESP_PWR_LVL_N12 == -12 dBm. Range typically ~3-8 m through walls.
//    If you need more reach, bump to N9 (-9), N6 (-6), N3 (-3), or N0 (0 dBm).
//    Each step roughly doubles the line-of-sight range.
void setupBLE()
{
  BLEDevice::init(BLE_DEVICE_NAME);
  // Low TX power on the advertising channel only (default applies to all roles).
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N12);

  pAdvertising = BLEDevice::getAdvertising();

  // Configure minimal, non-connectable advertising payload
  BLEAdvertisementData advData;
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advData.setName("Home"); // case MUST match BEACON_NAME on the collar
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
  Serial.println("[BLE] Advertising started: name='Home' tx_pwr=-12dBm (short-range)");
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

// Boot sequence. Order matters — some peripherals depend on others.
//
//   1. Serial up (115200) for diagnostics
//   2. Vext rail ON (powers GPS + TFT)
//   3. TFT init (so subsequent boot errors are visible without USB)
//   4. LEDs + BLE beacon
//   5. Wi-Fi join (boot loops on failure to avoid a stranded base station)
//   6. mDNS responder (so cattracker.local resolves on the LAN)
//   7. ArduinoOTA (after Wi-Fi is up)
//   8. LittleFS mount
//   9. Load persisted home location from /home_location.json
//   10. Register HTTP routes
//   11. WebSocket server up (port 81)
//   12. LoRa init + start RX
//   13. GPS UART up (the receiver's own GPS — feeds the MyDevice marker)
void setup()
{
  Serial.begin(115200);
  // V3 diag: a 3-second pause + explicit flush so the user has time to attach
  // the serial monitor and see our first prints even if something crashes
  // later in setup. Safe to remove once boot is reliable.
  delay(3000);
  Serial.println();
  Serial.println("[BOOT] ==================================================");
  Serial.println("[BOOT] Starting setup...");
  Serial.printf("[BOOT] BluePaws Receiver firmware v%s\n", BLUEPAWZ_VERSION);
  Serial.println("[BOOT] ==================================================");
  Serial.flush();

  // V3: power up Heltec V2's external rail (GPS + TFT) BEFORE touching any
  // peripheral on it. Vext is active-LOW. Without this the UC6580 stays dark
  // and the ST7735 won't respond to init.
  Serial.println("[BOOT] Step 1/13: Vext rail on");
  Serial.flush();
  heltecV2_enableVext();

  // V3: bring up the onboard TFT next so any boot errors below are visible
  // on-screen without needing the serial monitor.
  Serial.println("[BOOT] Step 2/13: TFT init");
  Serial.flush();
  tftBegin();

  Serial.println("[BOOT] Step 3/13: LEDs");
  Serial.flush();
  // DO NOT touch LED_BUILTIN on this board. The arduino-esp32
  // heltec_wifi_lora_32_V3 variant header (which our custom V2 board JSON
  // inherits from) defines LED_BUILTIN = 35 — but on the Wireless Tracker V2
  // GPIO 35 is the GPS reset line. Driving LED_BUILTIN puts the GPS into
  // reset and we get zero NMEA bytes in loop(). Use LORA_LED (GPIO 18, the
  // actual white indicator LED per the V2 schematic netlist NL18) instead.
  pinMode(LORA_LED, OUTPUT);
  digitalWrite(LORA_LED, LOW);
  Serial.println("[BOOT] Step 4/13: BLE beacon");
  Serial.flush();
  setupBLE();

  Serial.println("[BOOT] Step 5/13: WiFi connect");
  Serial.printf("[BOOT]   SSID='%s'\n", WIFI_SSID);
  Serial.flush();
  // V3: handle open networks robustly. arduino-esp32's WiFi.begin(ssid, "")
  // behaviour is inconsistent across versions — the single-argument form is
  // the canonical "no password, open AP" path. Strlen check covers the
  // "user pasted an empty string into secrets.h" case.
  if (strlen(WIFI_PASSWORD) == 0)
  {
    Serial.println("[BOOT]   (no password — assuming open network)");
    WiFi.begin(WIFI_SSID);
  }
  else
  {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
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
    // V3 diag: DO NOT restart on WiFi failure. The previous behaviour was
    // ESP.restart() which produced an opaque boot loop with no diagnostic
    // signal. Just log and carry on — the periodic checkWiFiConnection()
    // in loop() will keep trying. Means the TFT and LoRa still work even
    // without WiFi, and the user can see what went wrong via serial.
    Serial.println("\n[WIFI] ❌ Failed to connect — continuing in offline mode.");
    Serial.println("[WIFI]    Check WIFI_SSID / WIFI_PASSWORD in include/secrets.h");
    Serial.println("[WIFI]    Reconnect attempts will continue in loop().");
    isWiFiConnected = false;
  }

  // mDNS, ArduinoOTA, web server: all WiFi-dependent. Skip if WiFi is
  // down so a bad SSID doesn't take down the rest of the receiver
  // (TFT + LoRa + BLE still come up so we can at least see telemetry
  // and debug). The periodic checkWiFiConnection() in loop() will
  // eventually call MDNS.begin + the others if WiFi comes up later.
  if (!isWiFiConnected)
  {
    Serial.println("[BOOT] Skipping mDNS/OTA/HTTP — WiFi not up.");
    Serial.println("[BOOT] LoRa + TFT + BLE will still run.");
  }

  Serial.println("[BOOT] Step 6/13: mDNS");
  Serial.flush();
  // Initialize mDNS (only useful when WiFi is up)
  if (isWiFiConnected && MDNS.begin("cattracker"))
  {
    Serial.println("[mDNS] mDNS responder started. Access via http://cattracker.local");
  }
  else if (isWiFiConnected)
  {
    Serial.println("[mDNS] ❌ Failed to start mDNS responder");
  }

  // ───────────── ArduinoOTA (V3) ─────────────
  // Push firmware over WiFi from PlatformIO with:
  //   pio run -t upload --upload-port cattracker.local
  // (platformio.ini sets upload_protocol = espota.) No password by default;
  // if you want one, call ArduinoOTA.setPassword("...") before begin().
  Serial.println("[BOOT] Step 7/13: ArduinoOTA");
  Serial.flush();
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
  if (isWiFiConnected) {
    ArduinoOTA.begin();
    Serial.println("[OTA] ArduinoOTA ready — upload to cattracker.local");
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
  server.on("/version", HTTP_GET, handleGetVersion);        // Firmware version string
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

  // Flash the LoRa indicator LED 5 times to signal setup complete.
  // Using LORA_LED (GPIO 18 = the V2's actual white LED) and emphatically
  // NOT LED_BUILTIN — see the long comment in Step 3 above for why.
  Serial.println("[BOOT] Setup complete - signaling ready");
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(LORA_LED, HIGH);
    delay(100);
    digitalWrite(LORA_LED, LOW);
    delay(100);
  }
}

// Main super-loop. No FreeRTOS scheduler in use on the receiver —
// everything runs in order, fast enough to keep up with all subsystems.
//
// Per-pass cost budget (typical):
//   checkWiFiConnection         ~0.01 ms (cached)
//   server.handleClient          ~0.5 ms (HTTP idle), spikes on requests
//   webSocket.loop               ~0.5 ms idle, spikes on messages
//   ArduinoOTA.handle            ~0.05 ms idle
//   tftRefresh                   ~5 ms but only every 1000 ms (1 Hz)
//   handleLoRaPacket             ~30-60 ms when a packet arrived, 0 otherwise
//   processCommandQueue          0 unless rate-gate cleared AND queue non-empty
//   handleDeviceOwnGPS           ~1-5 ms when GPS bytes pending
//
// Total typical: <2 ms, spiking to ~100 ms when packet handling + WS
// broadcast + command TX coincide.
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
  tftRefresh();        // V3: ~1Hz status panel on Heltec V2 onboard TFT

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

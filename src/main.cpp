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
#include <esp_wifi.h>
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
// V3.6.0: keyed by device_id (immutable UID), NOT friendly name. The
// friendly name is a mutable display label carried inside each payload's
// "name" field. Two collars could share a name with zero collisions.
std::map<uint16_t, String> catPayloads;

// ─────────────────────────────────────────────────────────────────────
// V3.4.0 WEB-UI STATE PERSISTENCE
//
// Problem this solves: previously catPayloads/nodeStates lived only in
// RAM and the web client only populated markers from LIVE WebSocket
// pushes — so a page refresh, a new client, or a receiver reboot showed
// a blank map until each collar next reported (up to 5 min away).
//
// Two persistence mechanisms:
//   1. Trail ring buffer — last N positions per cat, served via /data so
//      the breadcrumb trail (not just the single latest dot) redraws on
//      reload.
//   2. LittleFS snapshot (/state.json) — catPayloads + trails + node
//      modes are written (debounced) so state survives a power-cut, and
//      reloaded on boot. Debounce keeps flash wear negligible.
// ─────────────────────────────────────────────────────────────────────
#define TRAIL_MAX_POINTS 10           // positions kept per cat for trails
#define STATE_FILE "/state.json"      // LittleFS snapshot path
#define STATE_SAVE_DEBOUNCE_MS 30000UL // ≤1 write / 30 s (flash-wear guard)

struct TrailPoint { float lat; float lon; };
std::map<uint16_t, std::vector<TrailPoint>> catTrails; // keyed by device_id (UID)

static bool g_stateDirty = false;       // set on any state change
static uint32_t g_lastStateSaveMs = 0;  // last LittleFS write time

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

// ───────────── KCT8103L RF front-end module (FEM) control ─────────────
// v3.9.0 — THE fix for "base won't transmit". The HTIT-Tracker_V2.3 does NOT
// drive its antenna straight from the SX1262: it routes through an external
// PA + LNA + T/R-switch module (KCT8103L) powered by a TLV75733 LDO. These
// lines MUST be driven or the PA never engages — TX merely leaks through the
// module (a "tiny emission" on a spectrum analyser) while RX still works via
// the module's idle path. That's why the base received fine but never put a
// command on the air. GPIOs decoded from the HTIT-Tracker_V2.3 schematic, and
// cross-checked: the same ESP32-symbol column gives LoRa_NSS=8 / LoRa_SCK=9 /
// Vext_Ctrl=3 — all matching the defines above — so these are trustworthy.
//   VFEM_Ctrl → enables the FEM's LDO (power)     : drive HIGH
//   PA_CSD    → FEM chip enable                   : drive HIGH
//   PA_CTX    → TX-path select                    : HIGH = TX, LOW = RX
//   PA_CPS    → hardwired via a 0R resistor (R31) — not software-controlled.
#define FEM_VCTRL 7
#define FEM_CSD   4
#define FEM_CTX   5

SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, LoRaSPI);

// Power up + enable the FEM and leave it in RX mode. MUST run before the radio
// transmits (called from setup() before lora.begin()). Without it the PA is
// unpowered and only leakage reaches the antenna.
static void femInit()
{
  pinMode(FEM_VCTRL, OUTPUT);
  digitalWrite(FEM_VCTRL, HIGH); // turn on the FEM's LDO (TLV75733) → FEM powered
  pinMode(FEM_CSD, OUTPUT);
  digitalWrite(FEM_CSD, HIGH);   // enable the FEM
  pinMode(FEM_CTX, OUTPUT);
  digitalWrite(FEM_CTX, LOW);    // default to the RX path (LNA)
  delay(20);                     // let the LDO settle before any RF
}

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

// ───────────── User button + TFT page navigation (V3.5.0) ─────────────
// The boot/PRG button (GPIO0) doubles as a runtime UI control. Held during
// RESET it still enters the ESP32-S3 bootloader (that's a hardware strap we
// can't and shouldn't change); during NORMAL operation it's a free input.
//   • single press → cycle to the next TFT page
//   • quick double press → jump straight back to the summary page
// Pages: 0 = summary (home), 1 = LoRa settings, 2.. = latest packet from
// each distinct device, one page per cat. Device pages appear/disappear as
// collars are heard, so the page count is computed live from catPayloads.
#define USER_BTN 0
static uint8_t g_tftPage = 0;            // current page index
static bool    g_tftPageChanged = true;  // true → full clear+redraw next refresh
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

// V3.1: collar BLE advertising prefix. Defined here near the top of the
// file so both tftRefresh() (early) and the BLE scan code (later) see it.
#define COLLAR_BLE_PREFIX "BLUEPAWZ-"

// V3.1 forward decls for roaming-mode UI. The full enum / globals live
// near the WiFi setup further down. Accessor functions wrap the
// complex globals (std::map etc.) so tftRefresh stays simple.
extern uint8_t netModeRaw();        // 0 = HOME, 1 = ROAMING
extern String  netModeApIpStr();    // AP IP as text, "" when not roaming
extern int     bleCollarCount();    // how many collars seen
extern String  bleStrongestCollarName();
extern int16_t bleStrongestCollarRssi();
extern uint32_t bleStrongestCollarLastSeenMs();

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

// Mount LittleFS at most once. Multiple setup stages use the filesystem, and
// LittleFS.begin() is not reliably safe to call twice.
static bool g_fsMounted = false;
static bool ensureFsMounted()
{
  if (g_fsMounted)
    return true;
  g_fsMounted = LittleFS.begin(true); // format on fail
  return g_fsMounted;
}

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
// V3.5.0: this is now the PAGE-0 (summary) renderer, dispatched by
// tftRefresh() below. Timing + screen-clear-on-page-change are handled by
// the dispatcher, so this just paints the summary in place (no flicker).
static void tftRenderSummary()
{
  char buf[32];
  tft.setTextSize(1);

  // Title bar
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(2, 2);
  snprintf(buf, sizeof(buf), "BluePaws v%-12s", BLUEPAWZ_VERSION);
  tft.print(buf);

  // WiFi status / IP — different display per network mode (V3.1 roaming).
  tft.setCursor(2, 14);
  if (netModeRaw() == 1 /* NET_ROAMING */)
  {
    // AP mode: show the AP IP prefixed with a R: tag in bright magenta
    // so the user can clearly tell we're in roaming mode from across a room.
    tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
    String apIp = netModeApIpStr();
    snprintf(buf, sizeof(buf), "R:%-18s", apIp.length() ? apIp.c_str() : "(no AP)");
  }
  else if (WiFi.status() == WL_CONNECTED)
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

  // Packets seen since boot — only shown in HOME mode. In ROAMING mode
  // the proximity widget takes over this row.
  if (netModeRaw() == 0 /* NET_HOME */)
  {
    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    tft.setCursor(2, 28);
    snprintf(buf, sizeof(buf), "Pkts: %-14u", tftMsgCount);
    tft.print(buf);
  }

  // V3.1: 'last cat' row — different display per mode.
  //
  // HOME mode    → last LoRa packet (existing behaviour)
  // ROAMING mode → graphical proximity meter ('cat finder'):
  //                  NOTHING: nothing heard for >60 s → bar hidden,
  //                           text says SEARCHING. The whole widget
  //                           area is blanked to reinforce "no signal".
  //                  COLD:    25% bar height, dark red
  //                  COOL:    50% bar height, amber
  //                  WARM:    75% bar height, yellow
  //                  HOT:    100% bar height, green, flashes
  //                          between bright green and white at 1 Hz.
  //
  // Layout in roaming mode (occupies y=28..78 left half + bar on right):
  //                ┌────────────────────┐
  //   y=28 - 38    │  FINDING:          │   bar
  //   y=40 - 50    │   Podge            │   ▓▓▓▓
  //   y=52 - 62    │  HOT -52dBm        │   ▓▓▓▓
  //   y=64 - 74    │  age: 0.3s         │   ▓▓▓▓
  //                └────────────────────┘
  //
  // The bar is a thermometer-style filled rectangle that grows upward
  // from the bottom. Outline always visible to anchor the widget; fill
  // height + colour change with signal strength.
  if (netModeRaw() == 1 /* NET_ROAMING */)
  {
    uint32_t now           = millis();
    uint32_t lastSeen      = bleStrongestCollarLastSeenMs();
    uint32_t silenceMs     = (lastSeen == 0) ? 0xFFFFFFFFu : (now - lastSeen);
    bool     haveLiveCollar = (lastSeen != 0) && (silenceMs < 60000UL);

    // Bar geometry — must NOT overlap the GPS status pill at y=68..78,
    // so we cap the bottom at y=66.
    const int barX      = 130;
    const int barY      = 24;     // top of the bar box
    const int barW      = 26;
    const int barMaxH   = 42;     // ends at y=66, one row above GPS pill
    const int barBottom = barY + barMaxH;

    // Draw / refresh the white outline (defines the widget visually)
    tft.drawRect(barX, barY, barW, barMaxH, ST77XX_WHITE);

    // Always blank the inside before redrawing the fill, so when the bar
    // shrinks (or disappears) we don't leave a tall fragment behind.
    tft.fillRect(barX + 1, barY + 1, barW - 2, barMaxH - 2, ST77XX_BLACK);

    if (!haveLiveCollar)
    {
      // ── NOTHING: no live collar, hide the bar and the data lines ──
      // Also blank the bar outline AND interior so the user sees just a
      // dark area where the meter was — reinforces "signal lost".
      tft.fillRect(barX, barY, barW, barMaxH, ST77XX_BLACK);

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(2, 28);
      snprintf(buf, sizeof(buf), "%-16s", "SEARCHING...");
      tft.print(buf);
      // Clear the other text rows so we don't leave stale collar info.
      tft.fillRect(2, 40, 120, 24, ST77XX_BLACK);
    }
    else
    {
      int16_t rssi = bleStrongestCollarRssi();
      // Trim 'BLUEPAWZ-' prefix from name for display.
      String name = bleStrongestCollarName();
      if (name.startsWith(COLLAR_BLE_PREFIX))
        name = name.substring(strlen(COLLAR_BLE_PREFIX));

      // 5-state bucket
      const char *tag;
      uint16_t fillCol;
      int      fillHpx;     // height of the fill (px from bottom of bar)
      uint16_t textCol;
      // Static toggles state-by-frame so HOT flashes
      static bool hotFlashPhase = false;

      if (rssi >= -55)
      {
        tag      = "HOT";
        // Flash green ↔ white at the 1 Hz refresh rate
        hotFlashPhase = !hotFlashPhase;
        fillCol  = hotFlashPhase ? ST77XX_WHITE : ST77XX_GREEN;
        fillHpx  = barMaxH - 2;
        textCol  = ST77XX_GREEN;
      }
      else if (rssi >= -70)
      {
        tag      = "WARM";
        fillCol  = ST77XX_YELLOW;
        fillHpx  = (barMaxH - 2) * 3 / 4;
        textCol  = ST77XX_YELLOW;
        hotFlashPhase = false;
      }
      else if (rssi >= -85)
      {
        tag      = "COOL";
        fillCol  = 0xFC00; // amber
        fillHpx  = (barMaxH - 2) * 2 / 4;
        textCol  = 0xFC00;
        hotFlashPhase = false;
      }
      else
      {
        tag      = "COLD";
        fillCol  = 0x8000; // dark red
        fillHpx  = (barMaxH - 2) * 1 / 4;
        textCol  = 0xC000; // medium red (text needs readability)
        hotFlashPhase = false;
      }

      // Fill the bar from the bottom upward
      int fillY = (barBottom - 1) - fillHpx;
      tft.fillRect(barX + 1, fillY, barW - 2, fillHpx, fillCol);

      // Text lines on the left
      tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
      tft.setCursor(2, 28);
      snprintf(buf, sizeof(buf), "%-16s", "FINDING:");
      tft.print(buf);

      tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      tft.setCursor(2, 40);
      snprintf(buf, sizeof(buf), "%-16s", name.c_str());
      tft.print(buf);

      tft.setTextColor(textCol, ST77XX_BLACK);
      tft.setCursor(2, 52);
      snprintf(buf, sizeof(buf), "%-4s %4ddBm  ", tag, rssi);
      tft.print(buf);

      // (No 'age' row — collapsed to keep everything above the GPS pill.)
    }
  }
  else
  {
    // ── HOME mode: existing 'last cat' row ──
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
  }

  // Home location + BLE state — only shown in HOME mode. In ROAMING the
  // proximity widget occupies these rows.
  if (netModeRaw() == 0 /* NET_HOME */)
  {
    tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
    tft.setCursor(2, 56);
    snprintf(buf, sizeof(buf), "Home set: %-10s", g_homeLocationSaved ? "yes" : "no");
    tft.print(buf);

    tft.setTextColor(bleEnabled ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK);
    tft.setCursor(2, 68);
    snprintf(buf, sizeof(buf), "BLE:%-3s            ", bleEnabled ? "on" : "off");
    tft.print(buf);
  }

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

// V3.5.0: PAGE 1 — LoRa PHY settings, so you can confirm the radio config
// on the device without a serial console. All values come from config.h
// (shared with the collar) except TX power, which the base station fixes
// at 22 dBm (see setOutputPower(22) in setup).
static uint8_t tftTotalPages()
{
  return (uint8_t)(2 + catPayloads.size()); // summary + LoRa + one per cat
}

static void tftRenderLoRa()
{
  char buf[32];
  tft.setTextSize(1);

  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(2, 2);
  snprintf(buf, sizeof(buf), "LoRa Cfg     %u/%u", (unsigned)(g_tftPage + 1), (unsigned)tftTotalPages());
  tft.print(buf);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(2, 16);
  snprintf(buf, sizeof(buf), "Freq %.1f MHz   ", (double)LORA_FREQ_MHZ);
  tft.print(buf);

  tft.setCursor(2, 28);
  snprintf(buf, sizeof(buf), "SF%-2d  BW %.0fkHz ", (int)LORA_SF, (double)LORA_BW_KHZ);
  tft.print(buf);

  tft.setCursor(2, 40);
  snprintf(buf, sizeof(buf), "CR 4/%d  CRC %s ", (int)LORA_CR, LORA_USE_CRC ? "on" : "off");
  tft.print(buf);

  tft.setCursor(2, 52);
  snprintf(buf, sizeof(buf), "Sync 0x%02X Pre %d ", (unsigned)LORA_SYNC_WORD, (int)LORA_PREAMBLE);
  tft.print(buf);

  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(2, 64);
  snprintf(buf, sizeof(buf), "TX pwr 22 dBm   ");
  tft.print(buf);
}

// V3.5.0: PAGE 2+ — latest packet from the devIdx-th distinct device in
// catPayloads (ordered map, so the index is stable per device set).
static void tftRenderDevice(uint8_t devIdx)
{
  auto it = catPayloads.begin();
  for (uint8_t i = 0; i < devIdx && it != catPayloads.end(); ++i)
    ++it;
  if (it == catPayloads.end())
    return; // device vanished between page-calc and render

  JsonDocument doc;
  if (deserializeJson(doc, it->second))
    return;

  char buf[32];
  tft.setTextSize(1);

  // Title: cat name (label) + page indicator. it->first is the UID key.
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(2, 2);
  const char *nm = doc["name"] | "";
  snprintf(buf, sizeof(buf), "%-10.10s %u/%u",
           (nm && nm[0]) ? nm : (String("Dev-") + it->first).c_str(),
           (unsigned)(g_tftPage + 1), (unsigned)tftTotalPages());
  tft.print(buf);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(2, 16);
  snprintf(buf, sizeof(buf), "ID %u  %-8s", (unsigned)(doc["device_id"] | 0),
           (const char *)(doc["mode"] | "?"));
  tft.print(buf);

  tft.setCursor(2, 28);
  snprintf(buf, sizeof(buf), "Stat %-11s", (const char *)(doc["status"] | "?"));
  tft.print(buf);

  tft.setCursor(2, 40);
  snprintf(buf, sizeof(buf), "Lat %.5f  ", (double)(doc["lat"] | 0.0));
  tft.print(buf);

  tft.setCursor(2, 52);
  snprintf(buf, sizeof(buf), "Lon %.5f  ", (double)(doc["lon"] | 0.0));
  tft.print(buf);

  // Distance + age of this fix (received_at is millis()-relative).
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(2, 64);
  double distM = doc["dist_m"] | 0.0;
  uint32_t rxAt = doc["received_at"] | (uint32_t)0;
  uint32_t ageS = (rxAt && millis() >= rxAt) ? (millis() - rxAt) / 1000 : 0;
  snprintf(buf, sizeof(buf), "%.0fm  %lus ago   ", distM, (unsigned long)ageS);
  tft.print(buf);
}

// V3.5.0: TFT dispatcher. Owns the 1 Hz rate limit and the clear-on-switch
// behaviour, then paints whichever page is active.
static void tftRefresh()
{
  // Force an immediate redraw on a page switch; otherwise cap at 1 Hz.
  if (!g_tftPageChanged && (millis() - tftLastRefresh < 1000))
    return;
  tftLastRefresh = millis();

  // Clamp the page in case a device disappeared since the last press.
  uint8_t total = tftTotalPages();
  if (g_tftPage >= total)
  {
    g_tftPage = 0;
    g_tftPageChanged = true;
  }

  if (g_tftPageChanged)
  {
    tft.fillScreen(ST77XX_BLACK); // one clean wipe on entry to a page
    g_tftPageChanged = false;
  }

  if (g_tftPage == 0)
    tftRenderSummary();
  else if (g_tftPage == 1)
    tftRenderLoRa();
  else
    tftRenderDevice(g_tftPage - 2);
}

// V3.5.1: poll the boot/PRG button (GPIO0, active-LOW via INPUT_PULLUP).
// SINGLE PRESS ONLY — each press advances to the next page; from the last
// page it wraps back to the summary. No double/long-press actions: a plain
// page-index loop, kept deliberately simple and predictable. Debounced.
//   summary → LoRa → dev1 → … → devN → summary → …
static void pollUserButton()
{
  static bool     lastLevel = HIGH;
  static uint32_t lastEdgeMs = 0;

  const uint32_t DEBOUNCE_MS = 40;

  bool level = digitalRead(USER_BTN);
  uint32_t now = millis();

  // Debounced falling edge = a press → advance one page (wrap at the end).
  if (lastLevel == HIGH && level == LOW && (now - lastEdgeMs) > DEBOUNCE_MS)
  {
    lastEdgeMs = now;
    uint8_t total = tftTotalPages();
    g_tftPage = (uint8_t)((g_tftPage + 1) % total);
    g_tftPageChanged = true;
  }
  lastLevel = level;
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

// ───────────── V3.1: Network mode (HOME / ROAMING) ─────────────
// Receiver runs in one of two network modes:
//   NET_HOME    — STA, joined the user's configured home WiFi. Normal
//                 gateway operation. BLE advertises the 'Home' beacon.
//   NET_ROAMING — Own open AP 'BluePaws-Roaming' for the user's phone
//                 to connect to while out searching for a lost cat.
//                 BLE switches from advertiser to scanner (looks for
//                 collar lost-mode beacons named 'BLUEPAWZ-*').
//
// Transition triggers:
//   HOME → ROAMING when home WiFi has been disconnected for
//                  ROAMING_SWITCH_TIMEOUT_MS (30 s) and reconnect
//                  attempts have failed.
//   ROAMING → HOME when a periodic scan detects the home SSID back
//                  on the air.
//
// The web server, WebSocket server, and HTTP routes don't care about
// which mode we're in — they bind to whichever interface is up.
enum NetMode { NET_HOME, NET_ROAMING };
NetMode  g_netMode = NET_HOME;
uint32_t g_disconnectStartMs = 0;        // when we first noticed disconnection
uint32_t g_lastHomeScanMs    = 0;        // last time we scanned for home SSID
IPAddress g_apIp;                         // saved softAP IP for UI display

#define ROAMING_SWITCH_TIMEOUT_MS    30000UL   // 30 s offline → switch to AP
#define ROAMING_HOMESCAN_INTERVAL_MS 60000UL   // 60 s between home-SSID scans
#define ROAMING_AP_SSID              "BluePaws-Roaming"
#define ROAMING_AP_CHANNEL           6
#define ROAMING_AP_MAX_CLIENTS       2

// Forward declarations for the mode-switch helpers (defined further down).
void switchToRoamingMode();
void switchToHomeMode();
bool homeSsidVisible();
bool startRoamingAccessPoint();

// Track WebSocket clients
uint8_t connectedClients = 0;

// Add timer for device GPS updates
unsigned long lastDeviceGPSUpdateTime = 0;
const unsigned long DEVICE_GPS_UPDATE_INTERVAL = 10000; // 10 seconds in milliseconds

// ───────────── BLE Beacon Config ─────────────
#define BLE_DEVICE_NAME "CAT_TRACKER_HQ"
BLEAdvertising *pAdvertising = nullptr;

// V3.1: BLE collar discovery (used in NET_ROAMING for cat-finder mode).
// Collars in lost mode advertise as 'BLUEPAWZ-<DEVICE_ID_INT>'. The
// receiver scans for those names and tracks RSSI with a simple EMA so
// the "getting warmer / colder" indicator is stable instead of jumping
// every frame.
struct CollarBleSighting
{
  String   name;          // advertised local name
  int16_t  rssiInst;      // last raw RSSI
  int16_t  rssiEMA;       // exponential moving average (alpha = 0.3)
  uint32_t lastSeenMs;    // millis() of most recent advertisement
  uint32_t sightingCount; // total advertisements seen since boot
};
std::map<String, CollarBleSighting> collarBleSeen;
BLEScan *pCollarScan = nullptr;  // set by bleStartCollarScan(), null otherwise
#define COLLAR_RSSI_EMA_ALPHA 30  // / 100 → 0.30; new sample weight
#define COLLAR_SCAN_DURATION_S 2
#define COLLAR_SCAN_INTERVAL_MS 10000UL
bool g_collarScanActive = false;
uint32_t g_lastCollarScanMs = 0;
unsigned long lastBLEAdvertTime = 0;
const unsigned long BLE_ADVERT_INTERVAL = 3000; // 5 seconds
bool bleEnabled = true;                         // BLE beacon control flag

// ───────────── Message Logging Config ─────────────
#define LOG_FILE_PATH "/messages.json"
// V3.9.0: 500 messages (~150 KB JSON) could no longer be parsed in the ESP32's
// fragmented heap — deserializeJson returned NoMemory on EVERY flush, which both
// stopped logging AND thrashed the heap (a likely cause of the WebSocket server
// dropping client connections). 150 keeps the file comfortably parseable.
#define MAX_LOG_MESSAGES 150     // Circular buffer size
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

std::map<uint16_t, NodeState> nodeStates; // keyed by device_id (UID); NodeState.deviceId holds the name label


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
void bleStartCollarScan();
void bleStopCollarScan();
void serviceCollarScan();
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

// V3.4.0 state-persistence helpers (defined later, used across the file)
void saveState();
void loadState();
void maybeSaveState();
void recordTrailPoint(uint16_t deviceId, float lat, float lon);

// Node state and command functions
void updateNodeState(const JsonDocument &doc);
void handleNodeResponse(const JsonDocument &doc);
void handleNodeStates();    // HTTP handler for /node-states
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
// V3.7.3: message attribution. Collars use their MAC-derived device_id
// (100-999); the base station itself is BASE_ID. Every logged message records
// src (who sent it) + dst (who it's for) + a msg_id, so an entry can be traced
// to the transmitter, the receiver, or an internally generated message.
// OTAP reserved device IDs (plain integers on the wire, shared with collars):
//   base/receiver = 1   broadcast = 999   collars = 100–998 (MAC-hashed)
#define BASE_ID 1
#define BROADCAST_ID 999
static uint32_t g_logSeq = 0; // fallback monotonic id for entries without one

// ═════════════════════════════════════════════════════════════════════
// OTAP — outbound command path + per-collar pending store (Phase 1)
// ═════════════════════════════════════════════════════════════════════
// Phase 1 keeps ONE pending command per collar (a std::map keyed by UID), not
// the full multi-command queue (that returns in Phase 2). A command is QUEUED
// when the user submits it (the collar is asleep), delivered on the collar's
// next presence/telemetry packet, and confirmed either by a matching ACK or by
// the collar's own telemetry echoing the new value (lost-ACK resilience).
enum CmdStatus { CMD_QUEUED, CMD_AWAITING_ACK, CMD_DELIVERED, CMD_FAILED };

struct PendingCmd
{
  uint16_t dest = 0;            // collar UID this command targets
  uint32_t msgId = 0;          // base-assigned message_id the collar must echo
  String json;                 // the fully-built command envelope, ready to TX
  String label;                // human action label for the UI ("rename"/"mode")
  // Confirm-by-telemetry: the command is also considered DELIVERED if the
  // collar's telemetry field `confirmField` later equals `confirmValue` (covers
  // a lost ACK). rename → ("name", new name); mode → ("mode", profile).
  String confirmField;
  String confirmValue;
  CmdStatus status = CMD_QUEUED;
  uint32_t sentAt = 0;         // millis() of the last TX attempt
  uint8_t immediateAttempts = 0; // # of immediate (non-presence) TX attempts (max 2)
};

static std::map<uint16_t, PendingCmd> g_pending; // one in-flight cmd per collar
static uint32_t g_baseMsgId = 1;                  // base's outbound message_id space

static const char *cmdStatusStr(CmdStatus s)
{
  switch (s)
  {
  case CMD_QUEUED:       return "queued";
  case CMD_AWAITING_ACK: return "awaiting_ack";
  case CMD_DELIVERED:    return "delivered";
  case CMD_FAILED:       return "failed";
  }
  return "queued";
}

void logMessage(const JsonDocument &doc, const String &type)
{
  if (!logFileInitialized)
    return;

  // Create log entry with GPS timestamp
  JsonDocument logEntry;
  logEntry["timestamp"] = getGPSTimestamp();
  logEntry["gps_time_valid"] = gps.time.isValid() && gps.date.isValid();
  logEntry["type"] = type; // "lora", "lora-ack", "mydevice", "event"

  // Copy all fields from original message
  JsonObjectConst sourceObj = doc.as<JsonObjectConst>();
  for (JsonPairConst kv : sourceObj)
  {
    logEntry[kv.key()] = kv.value();
  }

  // V3.7.3 + OTAP: source / destination / message-ID attribution, set AFTER the
  // copy so they are authoritative. PREFER the message's own explicit envelope
  // (source_id / destination_id) — present on telemetry, ack, nack, presence and
  // outbound commands — so every OTAP message attributes correctly. Fall back to
  // the legacy heuristic for older packets that only carry device_id/target_id:
  // "lora"/"lora-ack" are INBOUND from a collar; everything else ORIGINATES at
  // the base. This lets a log entry be traced to a collar (src = its UID), the
  // receiver (src = BASE_ID), or an internal/base-generated message.
  uint16_t aSrc = BASE_ID, aDst = BASE_ID;
  if (doc["source_id"].is<int>())
    aSrc = (uint16_t)doc["source_id"].as<int>();
  else if ((type == "lora" || type == "lora-ack") && doc["device_id"].is<int>())
    aSrc = (uint16_t)doc["device_id"].as<int>(); // inbound from this collar
  else
    aSrc = BASE_ID; // generated by the base (event / mydevice / outbound cmd)

  if (doc["destination_id"].is<int>())
    aDst = (uint16_t)doc["destination_id"].as<int>();
  else if (type == "lora" || type == "lora-ack")
    aDst = BASE_ID; // inbound telemetry is addressed to the base
  else if (doc["target_id"].is<int>())
    aDst = (uint16_t)doc["target_id"].as<int>();
  else if (doc["device_id"].is<int>())
    aDst = (uint16_t)doc["device_id"].as<int>();

  logEntry["src"] = aSrc;
  logEntry["dst"] = aDst;
  // Keep the packet/command's own msg_id when present; else adopt the OTAP
  // envelope's message_id; else stamp a monotonic sequential id so every entry
  // is individually addressable.
  if (doc["msg_id"].isNull())
  {
    if (doc["message_id"].is<int>())
      logEntry["msg_id"] = doc["message_id"];
    else
      logEntry["msg_id"] = ++g_logSeq;
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
    Serial.printf("[LOG] ❌ Failed to parse existing log: %s — truncating and starting fresh\n",
                  error.c_str());
    // The on-flash log is corrupt or too big to parse (NoMemory). NOTE:
    // initMessageLog() is a no-op once logFileInitialized is true, so the old
    // "recovery" never actually healed this — the file stayed broken and every
    // 60 s flush re-failed, thrashing the heap. Overwrite the file with an empty
    // skeleton HERE so the log self-heals instead of failing forever.
    File fresh = LittleFS.open(LOG_FILE_PATH, "w");
    if (fresh)
    {
      fresh.print("{\"device\":\"BluePawzReceiver\",\"messages\":[]}");
      fresh.close();
      Serial.println("[LOG] ✅ Log truncated — will rebuild from here");
    }
    else
    {
      Serial.println("[LOG] ❌ Could not truncate log file");
    }
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
  // V3.6.0: nodes are keyed by the immutable device_id (UID). A packet
  // with no device_id can't be tracked. Renames are now free — the UID
  // entry stays put and only its name label updates, so the old
  // "drop the stale-named entry" housekeeping is gone entirely.
  uint16_t incomingDevIdNum = 0;
  if (doc["device_id"].is<int>())
  {
    incomingDevIdNum = (uint16_t)doc["device_id"].as<int>();
  }
  if (incomingDevIdNum == 0)
  {
    return; // no UID → can't identify this node
  }

  // Friendly label (mutable). Falls back to "Device-<uid>" if absent.
  String deviceName = doc["name"].is<String>() ? doc["name"].as<String>()
                                                : (String("Device-") + incomingDevIdNum);
  if (deviceName == "MyDevice")
  {
    return; // Don't track base station's own device
  }

  // Get or create node state, keyed by UID.
  NodeState &state = nodeStates[incomingDevIdNum];
  // V3.7.2: the DISPLAYED name must change ONLY on genuine telemetry, never on
  // a command response. A set_name ACK echoes the requested NEW name, but the
  // collar may have applied it to RAM only and not persisted it — so updating
  // the card from the ACK shows the rename as "done" seconds after the user
  // submits, then it snaps back to the old name on the next real report (the
  // false positive being removed here). ACK / legacy status responses no
  // longer change the label; solicited pong telemetry is genuine telemetry and
  // may update it. (We still
  // initialise it on first contact so a brand-new node is never blank.)
  bool isResponsePacket = doc["ack"].is<String>() || (doc["status"] == "ok");
  if (!isResponsePacket || state.deviceId.length() == 0)
  {
    state.deviceId = deviceName; // the editable label — telemetry-confirmed only
  }
  state.deviceIdNum = incomingDevIdNum; // the immutable UID
  state.lastSeen = millis();
  g_stateDirty = true; // V3.4.0: node state changed → schedule a snapshot

  // V3.2.5: every telemetry packet now carries the collar's current
  // mode (transmitter doc["mode"] = g_currentMode). Pick it up here so
  // the C&C panel reflects reality. Only updates when the field exists AND we
  // weren't going to set it more authoritatively in the ACK branch
  // below (the ACK branch checks `ack:"mode"` and reads `profile`).
  if (doc["mode"].is<String>() && !doc["ack"].is<String>())
  {
    String reportedMode = doc["mode"].as<String>();
    if (reportedMode.length() > 0 && reportedMode != "unknown")
    {
      state.currentMode = reportedMode;
      state.modeKnown = true;
    }
  }

  // ACK/NACK routing happens before this path. Telemetry, including solicited
  // pong telemetry, reaches here and refreshes the collar state.
  broadcastNodeStates();
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
    node["device_id"] = state.deviceIdNum; // UID (identity / key)
    node["name"] = state.deviceId;          // editable label
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

// HTTP handler: GET /netmode — V3.1 roaming-mode state for the web UI.
// Returns { mode, ap_ip, sta_ip, collars:[{name,rssi,age_ms,count}] }.
// The UI polls this every couple of seconds to draw the proximity
// indicator when the receiver is in roaming mode.
void handleGetNetMode()
{
  JsonDocument doc;
  doc["mode"] = (netModeRaw() == 1) ? "roaming" : "home";
  if (netModeRaw() == 1)
  {
    doc["ap_ip"]   = netModeApIpStr();
    doc["ap_ssid"] = ROAMING_AP_SSID;
  }
  else
  {
    doc["sta_ip"] = WiFi.localIP().toString();
  }
  JsonArray collars = doc["collars"].to<JsonArray>();
  uint32_t now = millis();
  for (auto &p : collarBleSeen)
  {
    JsonObject c = collars.add<JsonObject>();
    String displayName = p.second.name;
    if (displayName.startsWith(COLLAR_BLE_PREFIX))
      displayName = displayName.substring(strlen(COLLAR_BLE_PREFIX));
    c["name"]   = displayName;
    c["rssi"]   = p.second.rssiEMA;
    c["age_ms"] = now - p.second.lastSeenMs;
    c["count"]  = p.second.sightingCount;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// HTTP handler: GET /version — returns BOTH firmware and filesystem
// versions. The web UI displays them side-by-side in the top-right
// info panel for at-a-glance troubleshooting.
//
// Why two versions:
//   - firmware_version comes from include/version.h compiled into the
//     binary that lives in app0/app1
//   - fs_version comes from data/version.json which is stored in the
//     LittleFS partition (uploaded via `pio run -t uploadfs`)
//
// V3.1.8 NOTE: we used to also emit a 'match' boolean and the UI would
// loudly warn on mismatch. Dropped — firmware-only releases (most of
// them) leave the FS untouched, so a mismatch is not a problem signal,
// just an information item. Users can read the two numbers and judge
// for themselves.
//
// The 'unknown' fallback for fs_version covers the case where
// /version.json doesn't exist on the LittleFS partition at all —
// usually means the filesystem hasn't ever been uploaded.
void handleGetVersion()
{
  JsonDocument doc;
  doc["firmware_version"] = BLUEPAWZ_VERSION;
  // V3.2.2: git commit hash baked in at build time by
  // scripts/inject_git_hash.py. Falls back to "unknown" if the build
  // happened outside a git checkout (e.g. release tarball).
#ifdef BLUEPAWZ_GIT_HASH
  doc["firmware_git_hash"] = BLUEPAWZ_GIT_HASH;
#else
  doc["firmware_git_hash"] = "unknown";
#endif

  // Filesystem version (the human semver) comes from /version.json, which
  // is hand-edited + committed in lockstep with include/version.h.
  String fsVer = "unknown";
  if (LittleFS.exists("/version.json"))
  {
    File f = LittleFS.open("/version.json", "r");
    if (f)
    {
      JsonDocument fsDoc;
      if (deserializeJson(fsDoc, f) == DeserializationError::Ok &&
          fsDoc["fs_version"].is<const char *>())
        fsVer = fsDoc["fs_version"].as<const char *>();
      f.close();
    }
  }

  // V3.6.3: the FS git hash now lives in /build_info.json, a build-time-
  // generated file that is GIT-IGNORED. It used to be stamped into the
  // tracked version.json, which created a perpetual "dirty after every
  // build" loop (a file can't hold its own commit's hash). build_info.json
  // is regenerated on every build and never committed, so it never shows
  // up as a pending change. See scripts/inject_git_hash.py.
  String fsHash = "unknown";
  if (LittleFS.exists("/build_info.json"))
  {
    File f = LittleFS.open("/build_info.json", "r");
    if (f)
    {
      JsonDocument biDoc;
      if (deserializeJson(biDoc, f) == DeserializationError::Ok &&
          biDoc["fs_git_hash"].is<const char *>())
        fsHash = biDoc["fs_git_hash"].as<const char *>();
      f.close();
    }
  }
  doc["fs_version"] = fsVer;
  doc["fs_git_hash"] = fsHash;

  // Backwards compat for any older UI that still reads "version":
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
    node["device_id"] = state.deviceIdNum; // UID (identity / key)
    node["name"] = state.deviceId;          // editable label
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
// OTAP — base TX helpers (re-arm-RX-safe) + delivery + status push
// ═════════════════════════════════════════════════════════════════════

// Transmit one JSON envelope to a collar, then IMMEDIATELY re-arm RX. The
// re-arm is critical: after transmit() the SX1262 is left in standby and will
// NOT hear the collar's ACK unless we call startReceive() again (the #1 risk in
// the plan — "base deaf during TX"). Synchronous; call only from loop() context
// (handlers run on the main loop, so the radio has a single owner here).
static bool sendLoRaJson(const String &json)
{
  // Switch the KCT8103L front-end into the TX path BEFORE transmitting, or the
  // PA never reaches the antenna (only leakage radiates). Return it to RX after,
  // so the base hears the collar's ACK. This is THE fix that got the base on the
  // air — proven on a HackRF + sniffer with the standalone TX test.
  digitalWrite(FEM_CTX, HIGH);   // FEM → TX path
  delayMicroseconds(50);         // brief antenna-switch settle
  int sb = lora.standby();
  // Byte-buffer transmit — identical to the proven pre-rip-out path
  // (ec6b62c transmitCommandAt: lora.transmit(cmd.buf, cmd.len)).
  int st = lora.transmit((uint8_t *)json.c_str(), json.length());
  digitalWrite(FEM_CTX, LOW);    // FEM → RX path (so we can hear the ACK)
  lora.startReceive(); // ALWAYS re-arm, even on error, or the base goes deaf
  // transmit() fires TxDone on DIO1, which our onReceive ISR also watches, so it
  // sets packetReceived and the next loop would "receive" our own just-sent
  // FIFO contents (a garbled echo). Clear the flag so we ignore that artifact;
  // a real inbound packet sets it again after startReceive.
  packetReceived = false;
  Serial.printf("[LoRa-TX] standby()=%d transmit()=%d %s: %s\n", sb, st,
                st == RADIOLIB_ERR_NONE ? "OK" : "ERROR", json.c_str());
  return st == RADIOLIB_ERR_NONE;
}

// Broadcast a command_status WS message in the EXACT shape the web UI's
// applyCommandUpdate() expects (Leaflet2_minimal.js routes "command_status"
// there). The UI prefers target_id, falls back to device_id; status strings are
// lowercase ("queued"/"awaiting_ack"/"delivered"/"failed").
static void pushCommandStatusWS(const PendingCmd &c, const char *reason = nullptr)
{
  JsonDocument d;
  d["type"] = "command_status";
  d["msg_id"] = c.msgId;
  d["status"] = cmdStatusStr(c.status);
  d["target_id"] = c.dest; // UI prefers target_id …
  d["device_id"] = c.dest; // … and falls back to device_id
  d["label"] = c.label;
  // Friendly-name hint for the UI, if this collar is known.
  auto it = nodeStates.find(c.dest);
  if (it != nodeStates.end() && it->second.deviceId.length())
    d["device"] = it->second.deviceId;
  if (reason)
    d["reason"] = reason;
  String out;
  serializeJson(d, out);
  webSocket.broadcastTXT(out);
  Serial.printf("[CMD] status push: msg_id=%lu dest=%u status=%s%s%s\n",
                (unsigned long)c.msgId, c.dest, cmdStatusStr(c.status),
                reason ? " reason=" : "", reason ? reason : "");
}

// Deliver the pending command (if any) for one collar, now that we know it is
// awake (we just heard a presence packet from it). Transmits the stored
// envelope. Re-sends on EVERY presence while the command is non-terminal
// (QUEUED or AWAITING_ACK): renames are idempotent, so a lost ACK simply
// self-heals on the next wake instead of stranding the command forever — and
// the entry is erased the moment a real ACK arrives or telemetry confirms the
// name, so retries are naturally bounded by reachability. (Phase 2 adds a retry
// cap + 12-min expiry.)
static void deliverPendingFor(uint16_t collarId)
{
  auto it = g_pending.find(collarId);
  if (it == g_pending.end())
  {
    Serial.printf("[OTAP] collar %u awake — no pending command\n", collarId);
    return;
  }
  PendingCmd &c = it->second;
  if (c.status == CMD_DELIVERED || c.status == CMD_FAILED)
  {
    Serial.printf("[OTAP] collar %u awake — pending cmd msg_id=%lu already %s, not re-sending\n",
                  collarId, (unsigned long)c.msgId, cmdStatusStr(c.status));
    return;
  }
  Serial.printf("[OTAP] collar %u awake — delivering cmd msg_id=%lu (was %s): %s\n",
                collarId, (unsigned long)c.msgId, cmdStatusStr(c.status), c.json.c_str());
  if (sendLoRaJson(c.json))
  {
    c.status = CMD_AWAITING_ACK;
    c.sentAt = millis();
    c.immediateAttempts = 2; // presence-scheduled delivery now owns it; stop the blind 10 s immediate retry
    Serial.println("[OTAP] delivered — AWAITING_ACK");
  }
  else
  {
    Serial.println("[OTAP] TX failed — will retry on next presence");
  }
  pushCommandStatusWS(c);
}

// Gap before the SECOND immediate command attempt. On cmd_send the base fires
// attempt 1 right away (the collar might be awake); if no ACK lands within this
// window, loop() fires attempt 2; after that it falls back to presence-scheduled
// delivery (deliverPendingFor on the collar's next wake).
#define IMMEDIATE_RETRY_MS 10000UL

// Called every loop() pass. Sends the 2nd immediate attempt ~10 s after the 1st
// for any command still unacked, then stops (presence delivery takes over).
static void processPendingRetries()
{
  uint32_t now = millis();
  for (auto &kv : g_pending)
  {
    PendingCmd &c = kv.second;
    if (c.status == CMD_AWAITING_ACK && c.immediateAttempts == 1 &&
        (now - c.sentAt) >= IMMEDIATE_RETRY_MS)
    {
      Serial.printf("[OTAP] immediate retry 2/2 for %u msg_id=%lu (no ACK in %lus)\n",
                    c.dest, (unsigned long)c.msgId, IMMEDIATE_RETRY_MS / 1000UL);
      sendLoRaJson(c.json);
      c.sentAt = now;
      c.immediateAttempts = 2; // done with immediate attempts → presence-scheduled now
      pushCommandStatusWS(c);
    }
  }
}

// Translate SHORT LoRa-wire keys back to the LONG keys the rest of the receiver
// (type-router, telemetry path, logMessage, catPayloads, AND the web UI) already
// expects — so the air frame is small but NOTHING downstream changes. Called
// once right after deserialize. Done value-first (read into a typed local, then
// remove + re-add) to avoid ArduinoJson pool-realloc aliasing that a direct
// doc[long] = doc[short] could hit.
static void expandWireKeys(JsonDocument &doc)
{
  static const struct { const char *s; const char *l; } INT_KEYS[] = {
      {"src", "source_id"}, {"dst", "destination_id"}, {"mid", "message_id"},
      {"seq", "msg_id"}, {"did", "device_id"}};
  for (auto &k : INT_KEYS)
  {
    if (!doc[k.s].isNull())
    {
      uint32_t v = doc[k.s].as<uint32_t>();
      doc.remove(k.s);
      doc[k.l] = v;
    }
  }

  // Current packets use src as both sender and immutable collar identity.
  // Keep accepting legacy did/device_id, but synthesize device_id when it is
  // absent so all downstream state/UI code remains unchanged.
  if (doc["device_id"].isNull() && doc["source_id"].is<int>())
  {
    uint16_t sourceId = (uint16_t)doc["source_id"].as<int>();
    const char *type = doc["type"] | "";
    if (sourceId != BASE_ID && sourceId != BROADCAST_ID &&
        (strcmp(type, "tel") == 0 || strcmp(type, "telemetry") == 0 ||
         strcmp(type, "ping") == 0 || strcmp(type, "presence") == 0 ||
         strcmp(type, "ack") == 0 ||
         strcmp(type, "nack") == 0))
    {
      doc["device_id"] = sourceId;
    }
  }

  // "tel" is the compact radio value. Internally and over WebSocket retain the
  // descriptive value expected by existing logs and browser code.
  if (doc["type"] == "tel")
    doc["type"] = "telemetry";
  else if (doc["type"] == "ping")
    doc["type"] = "presence";
  else if (doc["type"] == "CMD")
    doc["type"] = "command";

  static const struct { const char *s; const char *l; } STR_KEYS[] = {
      {"st", "status"}, {"md", "mode"}};
  for (auto &k : STR_KEYS)
  {
    if (doc[k.s].is<const char *>())
    {
      String v = doc[k.s].as<String>(); // owns a copy before we mutate the doc
      doc.remove(k.s);
      doc[k.l] = v;
    }
  }

  if (doc["mode"] == "dev")
    doc["mode"] = "developer";
  if (doc["status"] == "roam")
    doc["status"] = "roaming";
  else if (doc["status"] == "home")
    doc["status"] = "BLEHome";
}

// Convert Unix seconds from the compact collar wire payload back to the same
// human-readable UTC representation used before the wire-format change.
static String formatUnixUtc(uint32_t unixTime)
{
  time_t raw = (time_t)unixTime;
  struct tm utc;
  gmtime_r(&raw, &utc);
  char out[24];
  snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d:%02d",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  return String(out);
}

static uint32_t receiverUnixUtc()
{
  if (!gps.date.isValid() || !gps.time.isValid())
    return 0;
  return gpsToUnixTime(
      gps.date.year(), gps.date.month(), gps.date.day(),
      gps.time.hour(), gps.time.minute(), gps.time.second());
}

#define LAST_KNOWN_STALE_S 3600UL

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

  // Expand compact wire keys and normalize tel/src identity so
  // everything below — and the web UI — keeps working on the verbose names.
  expandWireKeys(doc);

  // New collars send GPS UTC as Unix seconds. Preserve the numeric source for
  // diagnostics, then restore the existing human-readable `time` field before
  // logging, persistence, or WebSocket broadcast. Legacy string time passes
  // through untouched.
  if (doc["time"].is<uint32_t>() || doc["time"].is<int>())
  {
    uint32_t unixTime = doc["time"].as<uint32_t>();
    doc["time_unix"] = unixTime;
    doc["time"] = formatUnixUtc(unixTime);
  }

  if (doc["status"] == "last")
  {
    doc["last_known"] = true;
    uint32_t nowUnix = receiverUnixUtc();
    uint32_t fixUnix = doc["time_unix"] | (uint32_t)0;
    if (nowUnix >= fixUnix && fixUnix != 0)
    {
      uint32_t ageS = nowUnix - fixUnix;
      doc["fix_age_s"] = ageS;
      doc["stale"] = ageS > LAST_KNOWN_STALE_S;
    }
  }

  // ── OTAP type-router ────────────────────────────────────────────────
  // The unified envelope carries an explicit "type". Route control packets
  // (presence / ack / nack) here and RETURN; anything else (telemetry, or a
  // legacy packet with no type) falls through to the existing path below.
  if (doc["type"].is<const char *>())
  {
    String mtype = doc["type"].as<const char *>();
    uint16_t src = doc["source_id"].is<int>() ? (uint16_t)doc["source_id"].as<int>()
                   : (doc["device_id"].is<int>() ? (uint16_t)doc["device_id"].as<int>() : 0);

    if (mtype == "presence")
    {
      // Collar just announced it is awake → deliver any queued command into
      // the wake window (BLE + GPS + stabilise).
      Serial.printf("[OTAP] presence from %u\n", src);
      logMessage(doc, "lora");
      deliverPendingFor(src);
      // Tell the web UI this collar is awake right NOW → drives the per-marker
      // "awake" indicator + its 60 s countdown (the collar can receive OTAP
      // commands during this window).
      {
        JsonDocument pres;
        pres["type"] = "presence";
        pres["device_id"] = src;
        String out;
        serializeJson(pres, out);
        webSocket.broadcastTXT(out);
      }
      return;
    }

    if (mtype == "ack" || mtype == "nack")
    {
      uint32_t mid = doc["message_id"].is<int>() ? (uint32_t)doc["message_id"].as<int>() : 0;
      const char *reason = doc["reason"].is<const char *>() ? doc["reason"].as<const char *>() : nullptr;
      logMessage(doc, "lora-ack");

      auto it = g_pending.find(src);
      if (it != g_pending.end() && it->second.msgId == mid)
      {
        if (mtype == "ack")
        {
          it->second.status = CMD_DELIVERED;
          Serial.printf("[OTAP] ACK matched msg_id=%lu from %u -> DELIVERED\n",
                        (unsigned long)mid, src);
          pushCommandStatusWS(it->second);
        }
        else
        {
          it->second.status = CMD_FAILED;
          Serial.printf("[OTAP] NACK msg_id=%lu from %u reason=%s -> FAILED\n",
                        (unsigned long)mid, src, reason ? reason : "(none)");
          pushCommandStatusWS(it->second, reason ? reason : "nack");
        }
        g_pending.erase(it);
      }
      else
      {
        Serial.printf("[OTAP] %s from %u msg_id=%lu — no matching pending cmd (ignored)\n",
                      mtype.c_str(), src, (unsigned long)mid);
      }
      return;
    }
    // type == "telemetry" (including normalized wire "tel") falls through.
  }

  // V3.2.5: response packets (mode ACK, status response, pong, set_name
  // ACK, set_geofence ACK) come back with `device` instead of `id`, so
  // the old outer gate that required `id`/`sender_name` was silently
  // discarding every ACK except set_name's (which uniquely also carries
  // `id`). That's why queued commands hung in AWAITING_ACK forever even
  // when the collar genuinely ACKed.
  //
  // Detect responses by their explicit markers:
  //   - "ack":<type>      mode / set_name / set_geofence
  //   - "status":"ok" + "mode" + no "lat"   get_status response
  //
  // Route responses to updateNodeState() (which handles the ACK branch
  // and runs markCommandDelivered) and bail out before the telemetry
  // path so we don't broadcast ACKs as position updates to web clients.
  // V3.7.1 FIX: a get_status RESPONSE is specifically status:"ok" (with a mode
  // and no position). The previous heuristic — "ANY status + mode + no lat" —
  // also matched real TELEMETRY that simply had no GPS fix yet
  // (status:"invalidGPSLoc"), so those check-ins were misrouted as responses
  // and dropped BEFORE notifyPosition(): the packet arrived (LED flickered)
  // but never reached the map tile or the web log. Match the response by its
  // actual "ok" marker so no-GPS telemetry still displays.
  bool isResponse = doc["ack"].is<const char *>() ||
                    (doc["status"] == "ok" &&
                     doc["mode"].is<const char *>() &&
                     !doc["lat"].is<float>() &&
                     !doc["latitude"].is<float>());

  if (isResponse)
  {
    // V3.6.0: responses carry device_id (UID) + name; updateNodeState keys
    // by device_id and reads the name label. No field patching needed.
    Serial.print("[LORA] response packet received: ");
    serializeJson(doc, Serial);
    Serial.println();
    logMessage(doc, "lora-ack");
    updateNodeState(doc);
    return;
  }

  // V3.6.0: telemetry is identified by device_id (the immutable UID). The
  // legacy id/sender_name fields are gone; name is just a display label.
  if (!error && doc["device_id"].is<int>())
  {
    uint16_t devId = (uint16_t)doc["device_id"].as<int>();

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

    // Status handling. We still canonicalise the THREE display states the UI's
    // marker-icon switch depends on (Home / Roaming / Offline → matching icon
    // assets). But every OTHER status is now passed through VERBATIM rather than
    // being collapsed to a generic "Error": the user wants the real reported
    // state (e.g. "invalidGPSLoc", and future descriptive states like
    // "low_battery"/"lora_error") to reach the log, map card, and web UI. The UI
    // shows the raw status text and falls back to the Error icon / grey dot for
    // any unrecognised value, so nothing breaks. (Phase 3 makes this fully
    // data-driven.)
    if (doc["status"].is<String>())
    {
      String originalStatus = doc["status"].as<String>();
      String lowerStatus = originalStatus;
      lowerStatus.toLowerCase();

      if (lowerStatus == "last")
      {
        bool stale = doc["stale"] | false;
        doc["status"] = stale ? "Last known (stale)" : "Last known";
      }
      else if (lowerStatus.indexOf("home") != -1)
      {
        doc["status"] = "Home";
      }
      else if (lowerStatus.indexOf("roaming") != -1 ||
               lowerStatus.indexOf("out") != -1 ||
               lowerStatus.indexOf("ok") != -1 ||
               lowerStatus.indexOf("normal") != -1 ||
               lowerStatus.indexOf("outanabout") != -1)
      {
        doc["status"] = "Roaming"; // V3.1.9: was 'Out'. Wire inputs ('roaming','outanabout','out',…) canonicalised for the icon switch.
      }
      else if (lowerStatus.indexOf("offline") != -1)
      {
        doc["status"] = "Offline";
      }
      // else: leave doc["status"] exactly as the collar sent it (e.g.
      // "invalidGPSLoc"). No generic "Error" rewrite.
    }
    else
    {
      doc["status"] = "Unknown"; // no status field at all — be honest, not "Error"
    }

    String payload;
    serializeJson(doc, payload);
    bool storePayload = true;
    bool isLastKnown = doc["last_known"] | false;
    if (isLastKnown)
    {
      auto existing = catPayloads.find(devId);
      if (existing != catPayloads.end())
      {
        JsonDocument previous;
        if (deserializeJson(previous, existing->second) == DeserializationError::Ok &&
            previous["lat"].is<float>() && previous["lon"].is<float>())
        {
          uint32_t previousFix = previous["time_unix"] | (uint32_t)0;
          uint32_t retainedFix = doc["time_unix"] | (uint32_t)0;
          bool previousWasLastKnown = previous["last_known"] | false;
          storePayload = previousWasLastKnown && retainedFix >= previousFix;
        }
      }
    }
    if (storePayload)
      catPayloads[devId] = payload; // keyed by UID

    // V3.4.0: append to the per-cat trail ring buffer + flag state dirty
    // so the LittleFS snapshot + reload-survivable breadcrumbs stay current.
    if (!isLastKnown &&
        doc["lat"].is<float>() && doc["lon"].is<float>())
    {
      recordTrailPoint(devId, doc["lat"].as<float>(), doc["lon"].as<float>());
    }
    if (storePayload || !isLastKnown)
      g_stateDirty = true;

    serializeJsonPretty(doc, Serial);
    Serial.println();

    logMessage(doc, "lora");
    updateNodeState(doc);
    notifyPosition(doc);

    // Friendly label for the TFT + command logging (may be empty).
    String reporting = doc["name"].is<String>() ? doc["name"].as<String>()
                                                 : (String("Device-") + devId);
    tftLastCatName = reporting; // V3: surface on the V2 onboard TFT
    (void)reporting;

    // OTAP confirm-by-telemetry (lost-ACK resilience): telemetry is the LAST
    // packet of the collar's wake — it deep-sleeps the instant it goes out — so
    // we must NOT try to deliver a queued command here (the collar would be
    // asleep before it arrived, and the command would brick in AWAITING_ACK
    // with no re-delivery). Delivery happens on the PRESENCE packet at the
    // START of the next wake, where the collar is wide awake for the full
    // BLE+GPS window. What we DO here: if a still-pending command's expected
    // value now appears in this collar's telemetry (name for a rename, mode for
    // a profile change), the change actually stuck even if the ACK was lost →
    // mark DELIVERED. (Phase 2 adds a post-TX RX window for same-wake
    // telemetry-triggered delivery + retry/expiry.)
    {
      auto it = g_pending.find(devId);
      if (it != g_pending.end() &&
          it->second.status != CMD_DELIVERED && it->second.status != CMD_FAILED &&
          it->second.confirmField.length() && it->second.confirmValue.length())
      {
        const char *field = it->second.confirmField.c_str();
        if (doc[field].is<const char *>() &&
            it->second.confirmValue == doc[field].as<const char *>())
        {
          it->second.status = CMD_DELIVERED;
          Serial.printf("[OTAP] %s confirmed by telemetry (%s=%s) for %u -> DELIVERED\n",
                        it->second.label.c_str(), field, it->second.confirmValue.c_str(), devId);
          pushCommandStatusWS(it->second);
          g_pending.erase(it);
        }
      }
    }

    // OTAP pong: a SOLICITED telemetry reply (pong:true) confirms a pending
    // "ping" command for this collar — the ping has no telemetry field to match,
    // the reply itself is the confirmation.
    if (doc["pong"].is<bool>() && doc["pong"].as<bool>())
    {
      auto it = g_pending.find(devId);
      uint32_t replyMid = doc["message_id"].is<int>()
                              ? (uint32_t)doc["message_id"].as<int>()
                              : 0;
      if (it != g_pending.end() && it->second.label == "ping" &&
          it->second.msgId == replyMid &&
          it->second.status != CMD_DELIVERED && it->second.status != CMD_FAILED)
      {
        it->second.status = CMD_DELIVERED;
        Serial.printf("[OTAP] ping msg_id=%lu confirmed by telemetry from %u -> DELIVERED\n",
                      (unsigned long)replyMid, devId);
        pushCommandStatusWS(it->second);
        g_pending.erase(it);
      }
    }
  }
  else
  {
    Serial.println("[LORA] Invalid JSON or missing device_id");
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
  //
  // OTAP identity: the base station is reserved ID 1 (BASE_ID). Its own GPS
  // report now carries the unified envelope (type/source_id/destination_id +
  // device_id == BASE_ID) so an inspected packet/log entry clearly reads as
  // ID 1 = the receiver. We KEEP the legacy id:"MyDevice" string as the UI's
  // internal marker key (the web UI maps device_id 1 → the "MyDevice" marker).
  deviceLocation["type"]           = "telemetry";
  deviceLocation["source_id"]      = BASE_ID;
  deviceLocation["destination_id"] = BROADCAST_ID; // self-report, informational
  deviceLocation["device_id"]      = BASE_ID;
  deviceLocation["id"]             = "MyDevice";
  deviceLocation["lat"]            = g_homeLat;
  deviceLocation["lon"]            = g_homeLon;
  deviceLocation["status"]         = "Starting up";
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
    if (error)
      continue;

    // V3.4.0: attach the per-cat trail history so a reloading client can
    // redraw the full breadcrumb line, not just the latest dot. Shape:
    // "trail": [[lat,lon],[lat,lon],...] oldest→newest.
    auto it = catTrails.find(pair.first);
    if (it != catTrails.end() && !it->second.empty())
    {
      JsonArray tr = singleDoc["trail"].to<JsonArray>();
      for (auto &p : it->second)
      {
        JsonArray pt = tr.add<JsonArray>();
        pt.add(p.lat);
        pt.add(p.lon);
      }
    }
    array.add(singleDoc);
  }
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

// ─────────────────────────────────────────────────────────────────────
// V3.4.0 STATE PERSISTENCE (LittleFS)
// ─────────────────────────────────────────────────────────────────────

// Append one fix to a cat's trail ring buffer, capped at TRAIL_MAX_POINTS.
void recordTrailPoint(uint16_t deviceId, float lat, float lon)
{
  auto &tr = catTrails[deviceId];
  tr.push_back({lat, lon});
  while (tr.size() > TRAIL_MAX_POINTS)
    tr.erase(tr.begin());
}

// Serialize catPayloads (+ trails) and the persistent NodeState fields to
// /state.json. Called debounced from loop() via maybeSaveState(). Note:
// per-packet timestamps (received_at/last_seen) are millis()-relative and
// therefore meaningless after a reboot — we persist POSITION + MODE, which
// is what makes the map useful on restart; "last seen" simply shows stale
// until the collar's next packet. That's an acceptable trade.
void saveState()
{
  JsonDocument doc;

  JsonArray cats = doc["cats"].to<JsonArray>();
  for (auto &pair : catPayloads)
  {
    JsonDocument cd;
    if (deserializeJson(cd, pair.second))
      continue;
    auto it = catTrails.find(pair.first);
    if (it != catTrails.end() && !it->second.empty())
    {
      JsonArray tr = cd["trail"].to<JsonArray>();
      for (auto &p : it->second)
      {
        JsonArray pt = tr.add<JsonArray>();
        pt.add(p.lat);
        pt.add(p.lon);
      }
    }
    cats.add(cd);
  }

  JsonArray nodes = doc["nodes"].to<JsonArray>();
  for (auto &pair : nodeStates)
  {
    NodeState &s = pair.second;
    JsonObject n = nodes.add<JsonObject>();
    n["device_id"] = s.deviceIdNum; // UID (key)
    n["name"] = s.deviceId;          // editable label
    n["mode"] = s.currentMode;
    n["power"] = s.txPower;
    n["sleep"] = s.sleepInterval;
    n["mode_known"] = s.modeKnown;
  }

  File f = LittleFS.open(STATE_FILE, "w");
  if (!f)
  {
    Serial.println("[STATE] save failed: could not open " STATE_FILE);
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.printf("[STATE] saved %u cats, %u nodes → %s\n",
                (unsigned)catPayloads.size(), (unsigned)nodeStates.size(), STATE_FILE);
}

// Restore catPayloads (+ trails) and node modes from /state.json on boot.
// Best-effort: a missing or corrupt file just means we start empty.
void loadState()
{
  if (!LittleFS.exists(STATE_FILE))
  {
    Serial.println("[STATE] no saved state to restore");
    return;
  }
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f)
    return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    Serial.printf("[STATE] restore parse error: %s — ignoring\n", err.c_str());
    return;
  }

  for (JsonObject cd : doc["cats"].as<JsonArray>())
  {
    if (!cd["device_id"].is<int>())
      continue;
    uint16_t id = (uint16_t)cd["device_id"].as<int>();

    if (cd["trail"].is<JsonArray>())
    {
      std::vector<TrailPoint> tr;
      for (JsonArray pt : cd["trail"].as<JsonArray>())
      {
        if (pt.size() >= 2)
          tr.push_back({pt[0].as<float>(), pt[1].as<float>()});
      }
      if (!tr.empty())
        catTrails[id] = tr;
    }
    cd.remove("trail"); // keep the stored payload clean; /data re-adds trail
    String payload;
    serializeJson(cd, payload);
    catPayloads[id] = payload;
  }

  for (JsonObject n : doc["nodes"].as<JsonArray>())
  {
    if (!n["device_id"].is<int>())
      continue;
    uint16_t id = (uint16_t)n["device_id"].as<int>();
    NodeState &s = nodeStates[id];
    s.deviceIdNum = id;
    s.deviceId = (const char *)(n["name"] | "");
    s.currentMode = (const char *)(n["mode"] | "unknown");
    s.txPower = n["power"] | 0;
    s.sleepInterval = n["sleep"] | 0;
    s.modeKnown = n["mode_known"] | false;
    s.lastSeen = 0; // unknown across reboot (millis reset)
  }

  Serial.printf("[STATE] restored %u cats, %u nodes from %s\n",
                (unsigned)catPayloads.size(), (unsigned)nodeStates.size(), STATE_FILE);
}

// Debounced write: only touches flash when state changed AND at least
// STATE_SAVE_DEBOUNCE_MS has elapsed since the last write. Bounds flash
// wear to ≤1 write per debounce window regardless of telemetry rate.
void maybeSaveState()
{
  if (!g_stateDirty)
    return;
  if (millis() - g_lastStateSaveMs < STATE_SAVE_DEBOUNCE_MS)
    return;
  g_stateDirty = false;
  g_lastStateSaveMs = millis();
  saveState();
}

// V3.1: Switch from home/STA mode to roaming/AP mode. Tears down the
// STA connection, brings up the open access point. Web/WebSocket
// servers keep running and bind to the new AP interface automatically
// (HTTP and WS servers are mode-agnostic).
bool startRoamingAccessPoint()
{
  WiFi.mode(WIFI_AP);
  IPAddress apIp(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, gateway, subnet);
  bool ok = WiFi.softAP(ROAMING_AP_SSID, nullptr, ROAMING_AP_CHANNEL,
                        false, ROAMING_AP_MAX_CLIENTS);
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  g_apIp = WiFi.softAPIP();
  return ok;
}

void switchToRoamingMode()
{
  if (g_netMode == NET_ROAMING) return;
  Serial.println("[WIFI] ── Switching to ROAMING mode (own AP) ──");
  WiFi.disconnect(true);
  delay(100);
  bool ok = startRoamingAccessPoint();
  Serial.printf("[WIFI] AP up: SSID='%s' (open) IP=%s channel=%u HT20 max_clients=%u\n",
                ROAMING_AP_SSID, g_apIp.toString().c_str(),
                ROAMING_AP_CHANNEL, ROAMING_AP_MAX_CLIENTS);
  if (!ok)
  {
    Serial.println("[WIFI] WARNING: softAP() reported failure");
  }
  g_netMode        = NET_ROAMING;
  isWiFiConnected  = false;     // not on home WiFi any more
  g_lastHomeScanMs = millis();

  // V3.1: BLE role-swap. Stop Home beacon, start collar-finder scanner.
  bleStartCollarScan();

  JsonDocument ev;
  ev["type"]        = "net_mode";
  ev["mode"]        = "roaming";
  ev["event"]       = "wifi_roaming_on";
  ev["ap_ssid"]     = ROAMING_AP_SSID;
  ev["ap_ip"]       = g_apIp.toString();
  logMessage(ev, "event");
  notifyPosition(ev);
}

// V3.1: Switch from roaming/AP back to home/STA mode. Stops the AP and
// re-attaches to the configured home network. Called when a periodic
// SSID scan reveals the home network is reachable again.
void switchToHomeMode()
{
  if (g_netMode == NET_HOME) return;
  Serial.println("[WIFI] ── Switching back to HOME mode (STA) ──");
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASSWORD) == 0)
    WiFi.begin(WIFI_SSID);
  else
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Brief blocking wait so the next loop pass has WL_CONNECTED ready.
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    isWiFiConnected = true;
    Serial.printf("[WIFI] Reconnected to home: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    // g_netMode still says ROAMING here, so switchToRoamingMode() would
    // intentionally no-op. Restore the AP explicitly after the failed STA try.
    Serial.println("[WIFI] STA reconnect failed — restoring ROAMING hotspot");
    bool apOk = startRoamingAccessPoint();
    Serial.printf("[WIFI] Roaming AP restore: %s IP=%s\n",
                  apOk ? "OK" : "FAILED", g_apIp.toString().c_str());
    return;
  }
  g_netMode            = NET_HOME;
  g_disconnectStartMs  = 0;

  // V3.1: BLE role-swap back. Stop collar scanner, resume Home beacon.
  bleStopCollarScan();

  JsonDocument ev;
  ev["type"]     = "net_mode";
  ev["mode"]     = "home";
  ev["event"]    = "wifi_roaming_off";
  ev["sta_ip"]   = WiFi.localIP().toString();
  logMessage(ev, "event");
  notifyPosition(ev);
}

// V3.1: Active WiFi scan, return true if the configured home SSID
// appears in the results. Used in ROAMING mode to decide when to
// switch back to HOME.
bool homeSsidVisible()
{
  // STA scanning requires AP+STA mode. This path is only called when no phone
  // is attached, so changing modes and scanning cannot disrupt a live UI.
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks(false /*async*/, false /*hidden*/);
  bool found = false;
  for (int i = 0; i < n; i++)
  {
    if (WiFi.SSID(i) == WIFI_SSID)
    {
      Serial.printf("[WIFI] Home SSID '%s' visible (RSSI=%d)\n",
                    WIFI_SSID, WiFi.RSSI(i));
      found = true;
      break;
    }
  }
  WiFi.scanDelete();
  if (!found)
  {
    WiFi.mode(WIFI_AP);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  }
  return found;
}

void checkWiFiConnection()
{
  // ──────────────── ROAMING (AP) branch ────────────────
  if (g_netMode == NET_ROAMING)
  {
    // Never run a blocking channel scan while a phone is using the hotspot.
    // When idle, check once per minute and restore AP-only mode if not found.
    if (millis() - g_lastHomeScanMs >= ROAMING_HOMESCAN_INTERVAL_MS)
    {
      uint8_t clients = WiFi.softAPgetStationNum();
      if (clients == 0)
      {
        g_lastHomeScanMs = millis();
        Serial.printf("[WIFI] (roaming idle) scanning for '%s'...\n", WIFI_SSID);
        if (homeSsidVisible())
        {
          switchToHomeMode();
        }
      }
      else
      {
        g_lastHomeScanMs = millis();
        Serial.printf("[WIFI] Home scan deferred: %u hotspot client(s) connected\n", clients);
      }
    }
    return;
  }

  // ──────────────── HOME (STA) branch ────────────────
  if (WiFi.status() != WL_CONNECTED)
  {
    if (isWiFiConnected)
    {
      Serial.println("[WIFI] Connection lost");
      isWiFiConnected      = false;
      g_disconnectStartMs  = millis();   // start the 30s clock

      JsonDocument eventDoc;
      eventDoc["event"]       = "wifi_disconnected";
      eventDoc["description"] = "WiFi connection lost";
      logMessage(eventDoc, "event");
    }

    // If we've been disconnected for longer than the tolerance, switch
    // to roaming. This is the home → roaming auto-trigger.
    if (g_disconnectStartMs > 0 &&
        millis() - g_disconnectStartMs > ROAMING_SWITCH_TIMEOUT_MS)
    {
      Serial.printf("[WIFI] STA down for >%lu s — going ROAMING\n",
                    ROAMING_SWITCH_TIMEOUT_MS / 1000);
      switchToRoamingMode();
      return;
    }

    // Try a single non-blocking reconnect attempt per call.
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
      isWiFiConnected     = true;
      g_disconnectStartMs = 0;
      Serial.println("\n[WIFI] Reconnected!");
      Serial.println(WiFi.localIP());

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
//  - V3.2.1: bumped from N12 (-12 dBm) to N3 (-3 dBm) — field-reported
//    reach at -12 was too short to cover a typical house. -3 dBm is
//    ~3x the range without flaring out to the neighbour's yard. The
//    collar's HOME_RSSI_THRESHOLD_DBM = -65 still defines the real
//    geofence boundary; TX power just shifts where that boundary sits.
//    If still patchy, step up to N0 (0 dBm). Available levels:
//    N24,N21,N18,N15,N12,N9,N6,N3,N0,P3,P6,P9 — P9 (+9 dBm) is the
//    ESP32-S3 ceiling, but don't go above N0 without re-evaluating the
//    "am I actually inside the house?" guarantee.
void setupBLE()
{
  BLEDevice::init(BLE_DEVICE_NAME);
  // Low TX power on the advertising channel only (default applies to all roles).
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N3);

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

  // Advertise whenever the user-controlled BLE beacon flag is enabled.
  if (bleEnabled)
  {
    BLEDevice::startAdvertising();
    lastBLEAdvertTime = millis();
    Serial.println("[BLE] Advertising started: name='Home' tx_pwr=-12dBm (short-range)");
  }
  else
  {
    Serial.println("[BLE] Beacon initialised but advertising OFF");
  }
}

// V3.1 BLE callback: invoked for every advertisement seen during a scan.
// Only collar 'BLUEPAWZ-*' names get tracked. RSSI is smoothed with an EMA
// so the proximity bar in the UI doesn't twitch.
class CollarScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice dev) override
  {
    if (!dev.haveName()) return;
    String name = String(dev.getName().c_str());
    if (!name.startsWith(COLLAR_BLE_PREFIX)) return;

    int16_t rssi = dev.haveRSSI() ? dev.getRSSI() : -127;
    auto it = collarBleSeen.find(name);
    if (it == collarBleSeen.end())
    {
      CollarBleSighting s;
      s.name = name;
      s.rssiInst = rssi;
      s.rssiEMA = rssi;
      s.lastSeenMs = millis();
      s.sightingCount = 1;
      collarBleSeen[name] = s;
      Serial.printf("[BLE-scan] NEW collar '%s' rssi=%d\n", name.c_str(), rssi);
    }
    else
    {
      it->second.rssiInst = rssi;
      // EMA: new_avg = alpha * sample + (1-alpha) * old_avg
      // Implemented in integer arithmetic with /100 fixed point.
      int32_t newEma = (COLLAR_RSSI_EMA_ALPHA * (int32_t)rssi +
                       (100 - COLLAR_RSSI_EMA_ALPHA) * (int32_t)it->second.rssiEMA) / 100;
      it->second.rssiEMA = (int16_t)newEma;
      it->second.lastSeenMs = millis();
      it->second.sightingCount++;
    }
  }
};
static CollarScanCallbacks g_collarScanCb;

static void onCollarScanComplete(BLEScanResults results)
{
  (void)results;
  g_collarScanActive = false;
  if (pCollarScan)
    pCollarScan->clearResults();
  Serial.println("[BLE] Collar scan window complete");
}

// Configure periodic collar-finder scans. The old 94%-duty continuous scan
// competed heavily with the SoftAP for the ESP32-S3's shared 2.4 GHz radio.
void bleStartCollarScan()
{
  if (pCollarScan) return; // already configured

  // Stop our own advertising first — can't safely advertise + scan
  // on the same controller in this stack.
  if (pAdvertising)
  {
    BLEDevice::stopAdvertising();
  }

  pCollarScan = BLEDevice::getScan();
  pCollarScan->setAdvertisedDeviceCallbacks(&g_collarScanCb, true /*wantDuplicates*/);
  pCollarScan->setActiveScan(true);
  pCollarScan->setInterval(320);  // units of 0.625 ms → 200 ms
  pCollarScan->setWindow(80);     //                    → 50 ms (25% in-window duty)
  g_collarScanActive = false;
  g_lastCollarScanMs = millis() - COLLAR_SCAN_INTERVAL_MS;
  Serial.printf("[BLE] Collar scanner configured: %us every %lus\n",
                COLLAR_SCAN_DURATION_S, COLLAR_SCAN_INTERVAL_MS / 1000);
}

void serviceCollarScan()
{
  if (g_netMode != NET_ROAMING || !pCollarScan || g_collarScanActive)
    return;
  if (millis() - g_lastCollarScanMs < COLLAR_SCAN_INTERVAL_MS)
    return;

  g_lastCollarScanMs = millis();
  g_collarScanActive = pCollarScan->start(
      COLLAR_SCAN_DURATION_S, onCollarScanComplete, false);
  if (!g_collarScanActive)
    Serial.println("[BLE] Collar scan window failed to start");
}

// V3.1 forward-decl accessors — let tftRefresh / web handlers read the
// roaming-mode state without needing to know about the complex globals.
uint8_t  netModeRaw()                  { return (uint8_t)g_netMode; }
String   netModeApIpStr()              { return (g_netMode == NET_ROAMING) ? g_apIp.toString() : String(""); }
int      bleCollarCount()              { return collarBleSeen.size(); }
String   bleStrongestCollarName()
{
  String best; int16_t bestR = -127;
  for (auto &p : collarBleSeen) {
    if (p.second.rssiEMA > bestR) { bestR = p.second.rssiEMA; best = p.second.name; }
  }
  return best;
}
int16_t  bleStrongestCollarRssi()
{
  int16_t bestR = -127;
  for (auto &p : collarBleSeen) if (p.second.rssiEMA > bestR) bestR = p.second.rssiEMA;
  return bestR;
}
uint32_t bleStrongestCollarLastSeenMs()
{
  uint32_t newest = 0;
  for (auto &p : collarBleSeen) if (p.second.lastSeenMs > newest) newest = p.second.lastSeenMs;
  return newest;
}

// V3.1: stop the collar-finder scanner and resume Home-beacon advertising.
// Called when we leave NET_ROAMING and return to NET_HOME.
void bleStopCollarScan()
{
  if (!pCollarScan) return;
  if (g_collarScanActive)
    pCollarScan->stop();
  pCollarScan->clearResults();
  pCollarScan = nullptr;
  g_collarScanActive = false;

  if (bleEnabled && pAdvertising)
  {
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Collar scanner stopped, Home beacon resumed");
  }
  else
  {
    Serial.println("[BLE] Collar scanner stopped");
  }
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

    // OTAP: queue a command for a collar. The collar is (almost always) asleep,
    // so we DO NOT transmit here — we store it QUEUED and deliver on the
    // collar's next presence/telemetry packet (deliverPendingFor). Phase-1
    // supported parameter: "name" (rename). The envelope mirrors the collar's;
    // add more known keys here (and in the collar's apply loop) for Phase 2.
    if (commandType == "cmd_send")
    {
      uint16_t dest = doc["device_id"].is<int>() ? (uint16_t)doc["device_id"].as<int>() : 0;
      if (dest == 0)
      {
        Serial.println("[WS] cmd_send missing/invalid device_id — ignored");
        return;
      }

      PendingCmd c;
      c.dest = dest;
      c.msgId = g_baseMsgId++;
      c.status = CMD_QUEUED;

      // Build the LoRa command with SHORT wire keys (the collar parses these);
      // the UI's WS message still uses its own long-ish keys (device_id/name/
      // profile/ping), read below.
      JsonDocument cmd;
      cmd["type"] = "CMD";
      cmd["src"] = BASE_ID;
      cmd["dst"] = dest;
      cmd["mid"] = c.msgId;

      if (doc["name"].is<const char *>())
      {
        c.label = "rename";
        c.confirmField = "name"; // telemetry echoes the applied name back
        c.confirmValue = doc["name"].as<const char *>();
        cmd["name"] = c.confirmValue;
      }
      else if (doc["profile"].is<const char *>())
      {
        // Power/operating profile change. The collar echoes it in telemetry's
        // mode field, so confirm-by-telemetry watches "mode" (the EXPANDED key).
        c.label = "mode";
        c.confirmField = "mode";
        c.confirmValue = doc["profile"].as<const char *>();
        cmd["md"] = (c.confirmValue == "developer") ? "dev" : c.confirmValue;
      }
      else if (doc["ping"].is<bool>() || doc["ping"].is<int>())
      {
        // Ping: the collar answers with a solicited telemetry (pong) reply. No
        // telemetry-field to confirm against — the pong itself confirms it (see
        // the pong handler in the telemetry path).
        c.label = "ping";
        cmd["ping"] = true;
      }
      else
      {
        Serial.println("[WS] cmd_send with no recognised parameter — ignored");
        return;
      }
      serializeJson(cmd, c.json); // SHORT-key string actually transmitted

      // If a non-terminal command is already in flight for this collar, close
      // it out as FAILED("superseded") so the UI doesn't orphan its msg_id in
      // a perpetual "waiting" state (Phase 1 keeps ONE in-flight cmd per collar).
      {
        auto old = g_pending.find(dest);
        if (old != g_pending.end() &&
            old->second.status != CMD_DELIVERED && old->second.status != CMD_FAILED)
        {
          old->second.status = CMD_FAILED;
          Serial.printf("[WS] superseding in-flight cmd msg_id=%lu for %u\n",
                        (unsigned long)old->second.msgId, dest);
          pushCommandStatusWS(old->second, "superseded");
        }
      }

      g_pending[dest] = c; // one in-flight command per collar (Phase 1)
      Serial.printf("[WS] cmd_send queued for %u msg_id=%lu: %s\n",
                    dest, (unsigned long)c.msgId, c.json.c_str());
      // Log with LONG keys (consistent with inbound logs): re-parse the
      // short-key command and expand it.
      {
        JsonDocument logDoc;
        deserializeJson(logDoc, c.json);
        expandWireKeys(logDoc);
        logMessage(logDoc, "command");
      }
      pushCommandStatusWS(g_pending[dest]);

      // Immediate-delivery attempt 1 of 2 (user request): the collar is usually
      // asleep, but on the off chance it is awake RIGHT NOW, fire the command
      // immediately. If no ACK lands within IMMEDIATE_RETRY_MS (10 s), loop()'s
      // processPendingRetries() fires attempt 2; after that the command falls
      // back to presence-scheduled delivery (deliverPendingFor) until ACK'd.
      Serial.println("[OTAP] cmd_send: immediate delivery attempt 1/2");
      sendLoRaJson(g_pending[dest].json);
      g_pending[dest].status = CMD_AWAITING_ACK;
      g_pending[dest].sentAt = millis();
      g_pending[dest].immediateAttempts = 1;
      pushCommandStatusWS(g_pending[dest]);
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

  // V3.5.0: boot/PRG button as runtime page-cycle control. INPUT_PULLUP →
  // idle HIGH, pressed LOW. (Held at reset still enters the bootloader.)
  pinMode(USER_BTN, INPUT_PULLUP);

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

  // Mount filesystem with better error reporting.
  if (!ensureFsMounted())
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

  // V3.4.0: restore last-known cat positions / trails / node modes from the
  // previous session so the web UI + map are populated immediately on boot,
  // before any collar has re-reported.
  loadState();

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
  server.on("/node-states", HTTP_GET, handleNodeStates);    // Get node display state
  server.on("/home", HTTP_GET, handleGetHome);              // Get current home lat/lon
  server.on("/home", HTTP_POST, handleSetHome);             // Set & persist home lat/lon
  server.on("/version", HTTP_GET, handleGetVersion);        // Firmware version string
  server.on("/netmode", HTTP_GET, handleGetNetMode);        // V3.1: roaming mode + collar RSSI
  // V3.8.0: remote-command HTTP API removed (/send-command, /commands,
  // /command, /commands/clear) — the receiver no longer sends commands.
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

  // Power + enable the KCT8103L RF front-end BEFORE the radio. Without this the
  // PA stays off and TX only leaks through the module — the base receives fine
  // but never puts a packet on the air. sendLoRaJson() flips FEM_CTX to TX per
  // transmit; here we just power it up and leave it in RX. (Verified on a HackRF
  // + sniffer: this is what took the base from a tiny emission to full power.)
  femInit();
  Serial.println("[FEM] KCT8103L powered + enabled (VFEM_Ctrl,PA_CSD HIGH; PA_CTX=RX)");

  int state = lora.begin(LORA_FREQ_MHZ);
  Serial.printf("[LoRa] begin(%.1f) = %d %s\n", (double)LORA_FREQ_MHZ, state,
                state == RADIOLIB_ERR_NONE ? "OK" : "FAIL");
  // Do NOT call setDio2AsRfSwitch() on this board: the TX path is gated by the
  // external FEM (driven via FEM_CTX above), not by DIO2. Enabling it only
  // toggled DIO2 and made TX worse during debugging; leaving DIO2 default is the
  // proven config (the original pre-rip-out firmware, ec6b62c, used no DIO2 cfg).
  int pw = lora.setOutputPower(22); // base station: max power (mains-powered)
  Serial.printf("[LoRa] setOutputPower(22) = %d %s\n", pw,
                pw == RADIOLIB_ERR_NONE ? "OK" : "FAIL");
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

    // ── OTAP TX SELF-TEST ──────────────────────────────────────────────
    // Fire one packet at boot via the real TX path (sendLoRaJson, which drives
    // the FEM into TX) so a sniffer confirms the base radiates at full power on
    // startup. A nice proof-of-life that the RF front-end is alive.
    bool ok = sendLoRaJson("{\"type\":\"selftest\",\"source_id\":1,\"msg\":\"base-tx\"}");
    Serial.printf("[LoRa] *** TX SELF-TEST sent (%s) — watch the sniffer for {\"type\":\"selftest\"} ***\n",
                  ok ? "OK" : "ERROR");
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
  // V3.1: in NET_ROAMING the AP is up and we want to KEEP serving the
  // web UI, even though isWiFiConnected (which now means 'STA joined to
  // home network') is false. Only bail out if we're nominally HOME mode
  // and currently disconnected — i.e. still trying to recover, no UI to
  // serve, no point spinning. ArduinoOTA also doesn't run in roaming mode
  // (no point — the user's laptop probably isn't on the AP).
  if (g_netMode == NET_HOME && !isWiFiConnected)
  {
    Serial.println("[WIFI] ⚠️ No connection, waiting...");
    delay(1000);
    return;
  }

  // Handle HTTP server and WebSocket events
  server.handleClient();
  webSocket.loop();
  serviceCollarScan();
  ArduinoOTA.handle(); // V3: service incoming OTA firmware uploads
  pollUserButton();    // V3.5.0: boot button cycles TFT pages
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

  // OTAP: fire the 2nd immediate command attempt ~10 s after the 1st if still
  // unacked (then presence-scheduled delivery takes over).
  processPendingRetries();

  // Periodic flush of message log to LittleFS
  if (millis() - lastLogFlushTime >= LOG_FLUSH_INTERVAL)
  {
    flushMessageLog();
  }

  // V3.4.0: debounced snapshot of cat positions / trails / node modes so
  // the web UI survives a receiver reboot, not just a page refresh.
  maybeSaveState();

  // No periodic re-advertising; BLE continues advertising until explicitly stopped/started
}

void notifyPosition(const JsonDocument &doc)
{
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.broadcastTXT(jsonString); // Broadcast the JSON to all WebSocket clients
  Serial.println("[WS] Position updated: " + jsonString);
}

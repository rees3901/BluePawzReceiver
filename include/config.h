/*
  ┌─────────────────────────────────────────────────────────────┐
  │  CAT TRACKER - DEVICE CONFIGURATION                        │
  │  Shared between TX nodes and RX base station               │
  │  Keep this file IDENTICAL on both devices!                 │
  └─────────────────────────────────────────────────────────────┘
*/

#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────
// FIXED LoRa Parameters (NEVER CHANGE VIA REMOTE)
// ─────────────────────────────────────────────
// These must match between ALL devices to maintain communication
// Changing these requires physical reprogramming of all nodes

// V3.3.0 range-tuning — MUST stay byte-identical to the transmitter's
// config.h (BluePawzTransmitter/include/config.h). SF8/BW250 → SF9/BW125
// trades data-rate for ~+5.5 dB link budget (≈1.5–2× range) at ~4× airtime.
// Freq 868.0 = EU/UK ISM (was 915 = US). The base station transmits its
// downlink commands at +22 dBm (mains-powered, see setOutputPower(22) in
// main.cpp); on the 868.0–868.6 sub-band the legal limit is +14 dBm ERP,
// so retune to the 869.4–869.65 high-power sub-band if full compliance
// at max power matters. See the transmitter config.h for the full note.
//
// These MUST match on ALL nodes (freq, SF, BW, sync word, CRC, header
// mode, preamble). CR is auto-detected by RX via the explicit header.
#define LORA_FREQ_MHZ 868.0 // EU/UK 868MHz ISM (was 915 = US)
#define LORA_SF 9           // Spreading Factor (7-12). 9 = range/airtime balance
#define LORA_BW_KHZ 125.0   // Bandwidth (kHz). 125 narrows noise floor for +3dB
#define LORA_CR 5           // Coding Rate 4/5 (auto-detected by RX via header)
#define LORA_PREAMBLE 8     // Standard preamble; base RX is always listening
#define LORA_USE_CRC 1      // Enable CRC
#define LORA_SYNC_WORD 0x12 // Private network sync word

// LBT (Listen Before Talk) - Collision avoidance
#define LBT_ENABLED true
#define LBT_MAX_RETRIES 5
#define LBT_RETRY_DELAY_MIN_MS 50
#define LBT_RETRY_DELAY_MAX_MS 500

// ─────────────────────────────────────────────
// GPS & BLE Configuration
// ─────────────────────────────────────────────
#define GPS_COLD_START_TIMEOUT 60000 // 60s for initial cold start
#define GPS_WARM_START_TIMEOUT 20000 // 20s for subsequent warm starts
#define GPS_VALID_COUNT_REQUIRED 5   // Consecutive valid fixes needed
#define GPS_STABILISE_MS 15000       // Stabilization period (15s)

#define BLE_INITIAL_SCAN_S 10 // Initial BLE scan on wake
#define BLE_SCAN_WINDOW_S 3   // BLE scan window during GPS
#define BEACON_NAME "Home"    // BLE beacon device name
#define HOME_RSSI_THRESHOLD_DBM (-90)
// ─────────────────────────────────────────────
// Operating Mode Profiles
// ─────────────────────────────────────────────

struct OperatingMode
{
    const char *name;
    int8_t lora_power_dbm;           // TX power (2-22 dBm)
    uint16_t sleep_interval_s;       // Sleep duration between cycles
    uint8_t led_flash_count;         // LED flashes per TX success
    bool led_beacon_mode;            // Continuous LED beacon while awake
    uint16_t led_beacon_interval_ms; // Interval between beacon flashes
    uint8_t home_heartbeat_cycles;   // Home cycles before GPS heartbeat TX
};

// ─────────────────────────────────────────────
// Mode Definitions
// ─────────────────────────────────────────────

// NORMAL - Daily tracking, balanced performance
const OperatingMode MODE_NORMAL = {
    .name = "normal",
    .lora_power_dbm = 17,
    .sleep_interval_s = 300, // 5 minutes (will become 10 min in production)
    .led_flash_count = 5,
    .led_beacon_mode = false,
    .led_beacon_interval_ms = 0,
    .home_heartbeat_cycles = 10};

// POWERSAVE - Maximum battery life at home
const OperatingMode MODE_POWERSAVE = {
    .name = "powersave",
    .lora_power_dbm = 10,     // Minimum viable power
    .sleep_interval_s = 1200, // 20 minutes
    .led_flash_count = 5,     // Keep standard flash (negligible power)
    .led_beacon_mode = false,
    .led_beacon_interval_ms = 0,
    .home_heartbeat_cycles = 10};

// ACTIVE - Frequent updates for monitoring
const OperatingMode MODE_ACTIVE = {
    .name = "active",
    .lora_power_dbm = 17,
    .sleep_interval_s = 60, // 1 minute
    .led_flash_count = 5,
    .led_beacon_mode = false,
    .led_beacon_interval_ms = 0,
    .home_heartbeat_cycles = 5};

// LOST - Emergency mode with visual beacon
const OperatingMode MODE_LOST = {
    .name = "lost",
    .lora_power_dbm = 22,          // Maximum power for range
    .sleep_interval_s = 30,        // 30 seconds (still need battery conservation!)
    .led_flash_count = 10,         // More flashes on TX
    .led_beacon_mode = true,       // Enable continuous beacon
    .led_beacon_interval_ms = 2000, // Flash every 2 seconds
    .home_heartbeat_cycles = 3
};

// DEVELOPER - Rapid cycle for testing and diagnostics
const OperatingMode MODE_DEVELOPER = {
    .name = "developer",
    .lora_power_dbm = 14,
    .sleep_interval_s = 30,
    .led_flash_count = 3,
    .led_beacon_mode = false,
    .led_beacon_interval_ms = 0,
    .home_heartbeat_cycles = 2};

// Developer-mode button configuration (used by collar firmware)
#define DEV_MODE_BUTTON_PIN 21
#define DEV_MODE_DOUBLE_PRESS_MS 500

// ─────────────────────────────────────────────
// Lost Mode Safety
// ─────────────────────────────────────────────
#define LOST_MODE_MAX_DURATION_S 7200    // 2 hours (120 minutes)
#define LOST_MODE_FALLBACK_MODE "normal" // Revert to normal mode after timeout

// Geofence configuration (used by collar firmware)
#define GEOFENCE_DEFAULT_RADIUS_M 500.0
#define GEOFENCE_ESCALATE_MODE "active"
#define GEOFENCE_HYSTERESIS_M 20.0

// ─────────────────────────────────────────────
// Remote Command Protocol
// ─────────────────────────────────────────────

// Command structure (base station → node):
// {"cmd":"mode","profile":"lost"}
// {"cmd":"mode","profile":"normal"}
// {"cmd":"mode","profile":"active"}
// {"cmd":"mode","profile":"powersave"}
// {"cmd":"mode","profile":"developer"}
// {"type":"CMD","src":1,"dst":N,"mid":42,"ping":true}
// Ping produces immediate telemetry tagged pong:true.

// Response structure (node → base station):
// {"ack":"mode","profile":"lost","power":22,"sleep":30}
// {"status":"ok","mode":"normal","battery":3.7,"gps":"locked","uptime":3600}

// ─────────────────────────────────────────────
// Helper Function: Get Mode by Name
// ─────────────────────────────────────────────
inline const OperatingMode *getModeByName(const char *name)
{
    if (strcmp(name, "normal") == 0)
        return &MODE_NORMAL;
    if (strcmp(name, "powersave") == 0)
        return &MODE_POWERSAVE;
    if (strcmp(name, "active") == 0)
        return &MODE_ACTIVE;
    if (strcmp(name, "lost") == 0)
        return &MODE_LOST;
    if (strcmp(name, "developer") == 0)
        return &MODE_DEVELOPER;
    return &MODE_NORMAL; // Default fallback
}

#endif // CONFIG_H

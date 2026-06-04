// ═════════════════════════════════════════════════════════════════════
// BluePawz Receiver — firmware version
//
// Semantic versioning, MAJOR.MINOR.PATCH:
//
//   MAJOR   protocol or hardware-level generation. V3 = JSON wire
//             protocol + Heltec V2 hardware + remote-config era.
//             Bumping to V4 means breaking compatibility with the
//             existing fleet of collars.
//
//   MINOR   user-visible new features. e.g. battery telemetry,
//             geofencing, breadcrumb persistence, sound alerts.
//             Backwards-compatible with the same MAJOR collars.
//
//   PATCH   bug fixes, polish, refactors. No new features, no wire
//             format changes.
//
// Bump procedure:
//   1. Edit BLUEPAWZ_VERSION below.
//   2. Commit with a message that begins with the new version, e.g.
//        "v3.0.1: fix BLE beacon name case mismatch"
//   3. (Optional) tag the commit: `git tag v3.0.1 && git push --tags`
//
// The version is surfaced in three places at runtime:
//   - the TFT status panel (top-right of the title bar)
//   - the GET /version HTTP endpoint, JSON {"version":"x.y.z"}
//   - the web UI title bar (fetched from /version on page load)
//
// Keep this header SMALL — it gets included from main.cpp and we want
// rebuilds to be fast.
// ═════════════════════════════════════════════════════════════════════
#pragma once

// 3.7.3  message log gains src/dst/msg_id attribution (trace each entry to a
//        collar, the base, or an internal message); a rename no longer reports
//        "delivered" on the ACK — only once the collar's telemetry confirms it.
// 3.7.2  fix: stop the GUI showing a rename as "done" before the collar
//        confirms it. The displayed name now changes ONLY on real telemetry,
//        not on the set_name ACK (which can echo a name the collar applied to
//        RAM but never persisted).
// 3.7.1  fix: no-GPS telemetry (status:"invalidGPSLoc") was misrouted as a
//        get_status response and dropped before display — collar check-ins
//        without a fix never reached the map/log.
// 3.7.0  developer mode (BLE 'Home' beacon defaults off for debugging) +
//        lost-ACK rename resilience + honour the ACK ok field.
// 3.6.4  prior release.
#define BLUEPAWZ_VERSION "3.7.3"

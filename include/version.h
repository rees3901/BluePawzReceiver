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

#define BLUEPAWZ_VERSION "3.1.8"

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

// 3.9.0  OTAP Phase 1 (rename round-trip). Unified message envelope
//        (type/source_id/destination_id/message_id) shared collar↔base.
//        BASE_ID=1, BROADCAST_ID=999. Base regains a TX path (sendLoRaJson,
//        re-arms RX after every transmit), a per-collar pending-command store,
//        and a type-router for presence/ack/nack: a queued rename is delivered
//        on the collar's next presence/telemetry, confirmed by a matching ACK
//        OR by the collar's own telemetry echoing the new name (lost-ACK
//        resilient). Web UI: per-card rename input + live status badge.
// 3.8.1  cleanup: delete the now-dead command-lifecycle code (~900 lines) —
//        LoRaCommand queue, ACK/pong/status handling, send/transmit helpers,
//        /send-command + /commands handlers. updateNodeState is telemetry-only.
// 3.8.0  remote-command system removed. The collar is transmit-only and the
//        base is display-only: no ping / rename / mode / status / ACKs, no
//        command queue, no /send-command API, no per-card command controls.
//        Pure telemetry tracker.
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
#define BLUEPAWZ_VERSION "3.9.0"

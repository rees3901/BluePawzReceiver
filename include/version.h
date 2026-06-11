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

// 3.9.0  OTAP Phase 1 (rename round-trip).
//        *** TX FIX: drive the KCT8103L RF front-end module. The HTIT-Tracker
//        V2.3 routes the SX1262 through an external PA+LNA+switch (powered by a
//        TLV75733 LDO), with control lines VFEM_Ctrl=GPIO7, PA_CSD=GPIO4,
//        PA_CTX=GPIO5 (decoded from the schematic). The base was RX-only, so
//        these were never driven — the PA stayed off and TX only LEAKED through
//        the module (a tiny emission; RX worked via the idle path), so no
//        command ever reached the air. femInit() powers+enables the FEM at boot;
//        sendLoRaJson() flips PA_CTX to the TX path per transmit. Verified full
//        power on a HackRF + sniffer. (NOT setDio2AsRfSwitch — that made it
//        worse; DIO2 is left in its default state.) ***
//        Unified message envelope
//        (type/source_id/destination_id/message_id) shared collar↔base.
//        BASE_ID=1, BROADCAST_ID=999. Base regains a TX path (sendLoRaJson,
//        re-arms RX after every transmit), a per-collar pending-command store,
//        and a type-router for presence/ack/nack: a queued rename is delivered
//        on the collar's next presence/telemetry, confirmed by a matching ACK
//        OR by the collar's own telemetry echoing the new name (lost-ACK
//        resilient). Web UI: per-card rename input + live status badge.
//        Packet structure: telemetry now carries the SAME envelope as the
//        other messages (type:"telemetry" + source_id + destination_id); to fit
//        the 255 B LoRa cap in dev mode, heap/uptime_ms are dropped from OTA
//        telemetry (still on serial) and fw rides steady-state packets only.
//        The base reports its own GPS as the reserved ID 1 (source_id/device_id
//        = BASE_ID, type:"telemetry"); the UI maps device_id 1 → the "MyDevice"
//        self-marker so a packet/log entry reads unambiguously as "from ID 1".
//        Also: status now passes through verbatim (e.g. "invalidGPSLoc")
//        instead of collapsing to a generic "Error"; the message log self-heals
//        on a NoMemory parse (cap 500→150) instead of thrashing the heap every
//        flush; UI no longer polls the removed /commands endpoint; unknown-cat
//        icons use an inline SVG instead of 404ing on a missing _Marker_ file.
//        Awake indicator: a presence packet makes the base push a "presence" WS
//        message; the marker tile AND the C&C side panel show 💡 (awake,
//        reachable for OTAP) for 30 s, then revert to 💤 (asleep).
//        OTAP power-profile command: a second OTAP parameter "profile"
//        (powersave/normal/active/lost) rides the same generic apply loop. UI
//        gains a profile dropdown (confirms before LOST). Collar persists via
//        saveOperatingMode + updates TX power live; confirm-by-telemetry is now
//        generic (name OR mode). Collar also gains a 5 s post-telemetry RX grace
//        window (POST_TX_LISTEN_MS) so a command + its ACK land in the same wake
//        before deep sleep.
//        Touch-ups: cmd delivery is now 1 immediate attempt → 10 s gap → 2nd
//        attempt → presence-scheduled (was a back-to-back double-send). Fixed the
//        C&C side-panel card rendering (getIconForNode now URL-encodes its inline
//        SVG so the data URI can't break the <img> tag).
//        + 'developer' is now an OTAP-settable profile. + Ping: a ping command
//        makes the collar emit an immediate telemetry reply (pong:true) with its
//        current/last values; the base confirms the ping on that pong (UI Ping
//        button). + SHORT LoRa wire keys to cut airtime: src/dst/mid/seq/did/st/md
//        on the air; the base's expandWireKeys() translates them back to the long
//        keys so the UI, logs and internal code are UNCHANGED.
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

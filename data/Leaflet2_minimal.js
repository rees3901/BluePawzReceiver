// Minimal Leaflet2.js - Just basic map functionality
// V3: HOME_LOCATION is mutable. Default is overridden by GET /home on load
// and by `home_location` WebSocket broadcasts after the user saves a new value.
let HOME_LOCATION = [51.87378215701798, -2.239428653198173];

// Initialize the map with minimal configuration
const map = L.map("map", {
  center: HOME_LOCATION,
  zoom: 5,
  maxZoom: 23,
});

// V3.6.2: single source of truth for friendly-name labels, keyed by the
// immutable device_id. Both the live telemetry path (handleTelemetry) and
// the node-state path (handleNodeStateUpdate, which carries set_name ACKs)
// funnel name updates through here, so a rename reflects on the marker tile
// AND the C&C panel at the same time — no surface lags the other.
window.deviceNames = window.deviceNames || {};
window.setDeviceName = function (id, name) {
  id = String(id);
  if (!name) return;
  const changed = window.deviceNames[id] !== name;
  window.deviceNames[id] = name;
  if (!changed) return;
  // Refresh the marker-tile card title in place (the card persists across
  // renames because it's keyed by device_id, so its title must be updated).
  const titleEl = document.querySelector(`#marker-card-${id} .marker-card-name`);
  if (titleEl) {
    const isKnown = (typeof KNOWN_CATS !== "undefined") && KNOWN_CATS.includes(name);
    titleEl.textContent = (isKnown || id === "MyDevice") ? name : (name + " (ID " + id + ")");
  }
};

// ─────────────────────────────────────────────────────────────────────
// V3.4.0 client-side UI preferences (per-browser, localStorage).
// Cat DATA comes from the server (shared truth across all clients); these
// are personal VIEW prefs, so they live in localStorage and never go stale
// or leak between clients. Stored as one JSON object under 'bluepaws.prefs'.
// (Theme has its own pre-existing key in index.html, left as-is.)
// ─────────────────────────────────────────────────────────────────────
window.Prefs = {
  _key: "bluepaws.prefs",
  data: {},
  load() {
    try {
      this.data = JSON.parse(localStorage.getItem(this._key)) || {};
    } catch (e) {
      this.data = {};
    }
    return this.data;
  },
  get(k, d) {
    return k in this.data ? this.data[k] : d;
  },
  set(k, v) {
    this.data[k] = v;
    try {
      localStorage.setItem(this._key, JSON.stringify(this.data));
    } catch (e) {}
  },
};
window.Prefs.load();

// Trail length (points kept per cat). Honoured by the breadcrumb cap in
// handleTelemetry. Default 4.
window.MAX_TRAIL_POINTS = window.Prefs.get("trailLength", 4);

// Define multiple map layers
const osmLayer = L.tileLayer(
  "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
  {
    maxZoom: 19,
    attribution:
      '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  }
).addTo(map);

const satelliteLayer = L.tileLayer(
  "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
  {
    maxZoom: 19,
    attribution: "Tiles &copy; Esri",
  }
);

const topoLayer = L.tileLayer(
  "https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png",
  {
    maxZoom: 17,
    attribution:
      "Map data: &copy; OpenStreetMap contributors, SRTM | Map style: &copy; OpenTopoMap",
  }
);

// V3.2.0: Vector-tile layer via MapLibre GL JS, wrapped as a Leaflet
// layer by the maplibre-gl-leaflet plugin (L.maplibreGL). Uses the
// OpenFreeMap "Liberty" style — no API key, no documented rate limits,
// hosted by openfreemap.org. The plugin renders MapLibre's GL canvas
// underneath Leaflet's marker/overlay panes, so all our existing
// markers, popups, drag handlers etc. keep working unchanged.
// Guarded with typeof check: if the CDN-hosted maplibre-gl-leaflet
// plugin fails to load (SRI mismatch or offline) we silently skip
// adding the layer rather than crashing the whole map.
let vectorLayer = null;
if (typeof L.maplibreGL === "function") {
  vectorLayer = L.maplibreGL({
    style: "https://tiles.openfreemap.org/styles/liberty",
    attribution:
      '&copy; <a href="https://openfreemap.org">OpenFreeMap</a> | ' +
      '&copy; <a href="https://www.openmaptiles.org/">OpenMapTiles</a> | ' +
      "&copy; OpenStreetMap contributors",
  });
}

// Create layer control with all map options
const baseMaps = {
  "Street Map": osmLayer,
  Satellite: satelliteLayer,
  Topographic: topoLayer,
};
// Only expose the vector option if the plugin actually loaded.
if (vectorLayer) {
  baseMaps["Vector (OpenFreeMap)"] = vectorLayer;
}

// Add layer control to map (top-right corner)
L.control
  .layers(baseMaps, null, {
    position: "topright",
    collapsed: false, // Set to true to collapse the control
  })
  .addTo(map);

// V3.4.0: restore the user's last-selected base layer (osmLayer is the
// default-on layer above) and remember any future change. Leaflet fires
// 'baselayerchange' with the human-readable layer name from baseMaps.
(function () {
  const savedLayer = window.Prefs.get("baseLayer", null);
  if (savedLayer && baseMaps[savedLayer]) {
    Object.values(baseMaps).forEach((l) => {
      if (map.hasLayer(l)) map.removeLayer(l);
    });
    map.addLayer(baseMaps[savedLayer]);
  }
  map.on("baselayerchange", (e) => window.Prefs.set("baseLayer", e.name));
})();

// Define KNOWN_CATS
const KNOWN_CATS = ["Podge", "Macy", "Gizmo", "Simba", "MyDevice"];

// Breadcrumb trail colors for each cat
const BREADCRUMB_COLORS = {
  Podge: "#FF6B6B", // Red
  Macy: "#4ECDC4", // Turquoise
  Gizmo: "#FFD93D", // Yellow
  Simba: "#A78BFA", // Purple
  MyDevice: "#00CED1", // Dark Turquoise for the device
  generic: "#007bff", // Default blue
};

// Function to get breadcrumb color for a marker
function getBreadcrumbColor(id) {
  return BREADCRUMB_COLORS[id] || BREADCRUMB_COLORS["generic"];
}

// Essential variables
const dropdowns = new Set();
const markers = {};
const markerVisibility = {};
const autoCenter = {};
const markerLastUpdate = {}; // Track last update time for each marker
// Track which marker is being followed (only one at a time)
let followedMarkerId = null;
// Provide globals used by index.html helpers
window.breadcrumbs = window.breadcrumbs || {};
window.breadcrumbLines = window.breadcrumbLines || {};

// V3.1.7: HTML-escape user-derived strings before injecting them into
// innerHTML. The `id` and `status` fields originate from incoming LoRa
// packets — the collar firmware sanitises names via saveSenderName(),
// but the receiver also accepts any well-formed JSON on the air, so an
// attacker transmitting on the right freq/SF/sync word could in
// principle put <script> in an id. Cheap to defend against, no reason
// not to.
function escHtml(s) {
  if (s == null) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// V3.1.7: status → CSS colour mapping for the unknown-device fallback dot.
// Mirrors the four normalised statuses produced by the receiver's JSON
// handler. Used by both getMarkerIcon() and createMarkerCard() so the
// fallback look is consistent everywhere.
function statusColour(status) {
  switch (status) {
    case "Home":    return "#28a745"; // green
    case "Roaming": return "#007bff"; // blue (V3.1.9: was "Out")
    case "Offline": return "#6c757d"; // grey
    case "Error":   return "#dc3545"; // red
    default:        return "#6c757d";
  }
}

// V3.1.7: build a Leaflet divIcon for unknown collars. Pure HTML + CSS,
// no image file needed — solves the 'broken image link' issue when a
// device name doesn't match the KNOWN_CATS list (Podge/Macy/Gizmo/
// Simba/Carrie). The dot inherits the same four-status colour scheme
// as the labelled icons.
function unknownDeviceDivIcon(status) {
  const colour = statusColour(status);
  return L.divIcon({
    html: `<div style="width:22px;height:22px;background:${colour};border:2px solid #fff;border-radius:50%;box-shadow:0 1px 3px rgba(0,0,0,0.6);"></div>`,
    iconSize: [26, 26],
    iconAnchor: [13, 13],
    popupAnchor: [0, -13],
    className: 'unknown-device-marker' // empty class — overrides Leaflet's default
  });
}

// Basic marker icon function
function getMarkerIcon(id, status) {
  // Special handling for MyDevice - use Device_Marker.avif
  if (id === "MyDevice") {
    return L.icon({
      iconUrl: `/icons/Device_Marker.avif`,
      iconSize: [32, 32],
      iconAnchor: [16, 32],
      popupAnchor: [0, -32],
    });
  }

  // V3.6.0: `id` is the device_id (UID) key. Icon files are named by the
  // friendly NAME (e.g. Podge_Marker_Home.avif), so resolve the label.
  const name = (window.deviceNames && window.deviceNames[id]) || id;

  // V3.1.7: unknown collars get a CSS-only coloured dot rather than
  // pointing at a generic_Marker_*.avif file that doesn't exist. No
  // 404, no broken image, and the colour still conveys the four
  // normalised states (Home/Out/Offline/Error).
  if (!KNOWN_CATS.includes(name)) {
    return unknownDeviceDivIcon(status);
  }

  let iconStatus;
  // Map to simplified 4-state system
  switch (status) {
    case "Home":
      iconStatus = "Home";
      break;
    case "Roaming":
      iconStatus = "Outanabout"; // V3.1.9: status renamed to Roaming, icon filenames kept as Outanabout for asset stability
      break;
    case "Offline":
      iconStatus = "Offline";
      break;
    case "Error":
      iconStatus = "Error";
      break;
    default:
      iconStatus = "Error"; // Default fallback
  }

  return L.icon({
    iconUrl: `/icons/${name}_Marker_${iconStatus}.avif`,
    iconSize: [32, 32],
    iconAnchor: [16, 32],
    popupAnchor: [0, -32],
  });
}

// Store message history for each marker
const markerHistory = {};

// Create marker card dynamically
// ═══════════════════════════════════════════════════════════════════
// V3.9.0: collar "awake" indicator
// ═══════════════════════════════════════════════════════════════════
// Collars sleep between wakes. On waking, a collar sends a presence packet;
// the base forwards it over WS (markCollarAwake), and we light the marker
// tile's indicator for 60 s — the window the collar is reachable for OTAP
// commands. It flips back to "asleep" automatically when the window lapses.
const COLLAR_AWAKE_MS = 60000;
const collarAwakeUntil = {}; // id (string) → epoch ms the awake window ends

window.markCollarAwake = function (id) {
  collarAwakeUntil[id] = Date.now() + COLLAR_AWAKE_MS;
  applyAwakeIndicator(id);
};

function applyAwakeIndicator(id) {
  const el = document.getElementById(`awake-ind-${id}`);
  if (!el) return;
  const until = collarAwakeUntil[id] || 0;
  if (until > Date.now()) {
    const secs = Math.max(1, Math.ceil((until - Date.now()) / 1000));
    el.textContent = "💡";
    el.classList.add("awake");
    el.classList.remove("asleep");
    el.title = `Awake — presence received; reachable for commands (~${secs}s)`;
  } else {
    el.textContent = "💤";
    el.classList.add("asleep");
    el.classList.remove("awake");
    el.title = "Asleep — no presence in the last minute";
  }
}
window.applyAwakeIndicator = applyAwakeIndicator;

// Re-evaluate every 5 s so indicators flip back to "asleep" when the 60 s
// window lapses (and the countdown tooltip stays roughly current).
setInterval(function () {
  for (const id in collarAwakeUntil) applyAwakeIndicator(id);
}, 5000);

// One-time styles: dim/grey when asleep, bright + gentle pulse when awake.
(function injectAwakeStyles() {
  const s = document.createElement("style");
  s.textContent =
    ".awake-indicator{position:absolute;top:0;right:0;font-size:1.3em;line-height:1;cursor:help;transition:opacity .3s;}" +
    ".awake-indicator.asleep{opacity:.35;filter:grayscale(1);}" +
    ".awake-indicator.awake{opacity:1;animation:awakePulse 1.5s ease-in-out infinite;}" +
    "@keyframes awakePulse{0%,100%{transform:scale(1);}50%{transform:scale(1.25);}}";
  document.head.appendChild(s);
})();

function createMarkerCard(id, status) {
  if (dropdowns.has(id)) return;

  console.log(`Creating marker card for ${id} with status ${status}`);
  dropdowns.add(id);

  // Initialize message history
  if (!markerHistory[id]) {
    markerHistory[id] = [];
  }

  // V3.6.0: `id` is the device_id (UID) key. Resolve the friendly NAME for
  // the thumbnail filename + the display label.
  const name = (window.deviceNames && window.deviceNames[id]) || id;
  const isKnown = KNOWN_CATS.includes(name);
  let iconUrl;
  let iconIsInline = false;
  if (id === "MyDevice") {
    iconUrl = "/icons/Device_Marker.avif";
  } else if (isKnown) {
    iconUrl = `/icons/${name}_Marker_Home.avif`;
  } else {
    // Inline SVG dot, embedded as a data URI — no network round-trip,
    // no 404 possible. Colour matches the unknown-device map marker.
    const colour = statusColour(status);
    const svg = `<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>` +
                `<circle cx='12' cy='12' r='10' fill='${colour}' stroke='white' stroke-width='2'/>` +
                `</svg>`;
    iconUrl = "data:image/svg+xml;utf8," + encodeURIComponent(svg);
    iconIsInline = true;
  }
  // Display label: known collars show their friendly name; unknown collars
  // show the name (or "Device-<uid>") so the user can still identify them.
  const displayId = (isKnown || id === "MyDevice") ? name : (name + " (ID " + id + ")");

  const statusClass = status
    ? `status-${status.toLowerCase()}`
    : "status-offline";

  // Special styling for MyDevice
  const isMyDevice = id === "MyDevice";
  const myDeviceClass = isMyDevice ? " my-device-card" : "";
  const deviceLabel = isMyDevice
    ? '<span style="background: #007bff; color: white; padding: 2px 8px; border-radius: 12px; font-size: 0.8em; margin-left: 8px;">🏠 This Device</span>'
    : "";

  // V3.9.0: "awake" indicator (top-right of the tile). A collar is shown awake
  // for 60 s after its presence packet — i.e. reachable for OTAP commands right
  // now. The base (MyDevice) is always on, so it gets no sleep/wake indicator.
  const awakeIndicator = isMyDevice
    ? ""
    : `<span class="awake-indicator asleep" id="awake-ind-${id}" title="Asleep — no presence in the last minute">💤</span>`;

  const card = document.createElement("div");
  card.className = `marker-card ${statusClass}${myDeviceClass}`;
  card.id = `marker-card-${id}`;

  // V3.1.7: HTML-escape user-derived text content before injecting into
  // innerHTML. This is the primary XSS defence in this function.
  //
  // ASSUMPTION: device `id` is from the collar firmware path which
  // sanitises names via saveSenderName() — no quotes, no commas, no
  // backslashes, no control characters. That means raw `${id}` is safe
  // to interpolate into HTML attributes and onclick handler strings,
  // since the dangerous metacharacters (`"` `'` `\\`) can never appear.
  // We still escape for innerHTML *text content* so that:
  //   (a) future widening of the allowed char set doesn't open an XSS,
  //   (b) an attacker injecting raw LoRa JSON (bypassing the collar
  //       firmware) can't slip <script> into a name field — it would
  //       just render as literal text.
  const idEsc      = escHtml(id);
  const displayEsc = escHtml(displayId);
  const statusEsc  = escHtml(status || "Unknown");
  card.innerHTML = `
    <div class="marker-card-header" style="position:relative;">
      <img src="${iconUrl}" alt="${idEsc}" class="marker-card-icon" id="card-icon-${id}">
      <div class="marker-card-title">
        <h3 class="marker-card-name">${displayEsc}${deviceLabel}</h3>
        <p class="marker-card-status" id="card-status-${id}">${statusEsc}</p>
      </div>
      ${awakeIndicator}
    </div>
    <div class="marker-card-actions">
      <div class="button-row">
        <button class="marker-card-btn btn-jump" onclick="jumpToMarker('${id}')" title="Jump to Marker
Centers the map on this tracker's current location
Uses zoom level 18 for detailed street view">
          🎯 Jump
        </button>
        <button class="marker-card-btn btn-follow" id="follow-btn-${id}" onclick="toggleFollowMarker('${id}')" title="Follow Marker
Automatically centers map on this tracker when it moves
Only one tracker can be followed at a time">
          📌 Follow: OFF
        </button>
      </div>
      <button class="marker-card-btn btn-breadcrumb" id="breadcrumb-btn-${id}" onclick="toggleBreadcrumbsCard('${id}')" title="Breadcrumb Trail
Shows the last 4 GPS positions as a colored line
Helps visualize movement patterns and direction">
        📍 Trail: OFF
      </button>
      <button class="marker-card-btn btn-console" onclick="toggleConsoleCard('${id}')" title="Message Log
Displays raw GPS messages received from this tracker
Useful for debugging and seeing exact coordinates">
        📄 Message Log
      </button>
      ${
        id !== "MyDevice"
          ? `
      <div style="margin-top: 8px; padding-top: 8px; border-top: 1px solid #dee2e6;">
        <div style="font-size: 0.75em; font-weight: 600; color: #666; margin-bottom: 4px; text-transform: uppercase;">
          Remote Commands
        </div>
        <!-- V3.2.3: this slot is filled by window.refreshCardControls
             on every WS node-state push, using the SAME shared helper
             (window.buildRemoteControlsHTML) the C&C panel uses. That
             keeps the two surfaces identical and in lockstep — same
             options, same buttons, same disabled-while-pending state,
             same lost-mode confirm. Before the first node-state arrives
             the slot shows a placeholder. -->
        <div class="node-controls" id="card-controls-${id}" style="display: flex; gap: 4px; align-items: center; flex-wrap: wrap; margin-bottom: 4px;">
          <span style="font-size: 0.8em; color: #999;">Waiting for node state…</span>
        </div>
        <div id="card-mode-status-${id}" style="font-size: 0.75em; color: #666;">
          <strong>Current Status:</strong> <span id="card-mode-text-${id}">Waiting for data...</span>
        </div>
      </div>
      `
          : ""
      }
    </div>
    <div class="marker-card-console" id="console-${id}">
      <div class="console-messages" id="console-messages-${id}">
        No messages yet
      </div>
    </div>
  `;

  document.querySelector(".marker-cards-container").appendChild(card);

  // V3.9.0: reflect the current awake/asleep state immediately (covers a card
  // created while the collar is still inside its 60 s awake window).
  if (window.applyAwakeIndicator) window.applyAwakeIndicator(id);

  // V3.2.3: if node state is already cached for this device, fill the
  // controls slot immediately. Otherwise the next /node-states WS push
  // will fill it via window.refreshCardControls.
  if (id !== "MyDevice" && window.refreshCardControls && window.nodeStates && window.nodeStates[id]) {
    window.refreshCardControls(window.nodeStates[id]);
  }
}

// Jump to marker location
window.jumpToMarker = function (id) {
  if (markers[id]) {
    const latlng = markers[id].getLatLng();
    map.setView(latlng, 16);
    markers[id].openPopup();
  }
};

// Toggle breadcrumb trail
window.toggleBreadcrumbsCard = function (id) {
  const btn = document.getElementById(`breadcrumb-btn-${id}`);
  const isShowing = btn.classList.contains("active");

  if (isShowing) {
    hideBreadcrumbs(id);
    btn.classList.remove("active");
    btn.innerHTML = "📍 Trail: OFF";
    setTrailPref(id, false);
  } else {
    showBreadcrumbs(id);
    btn.classList.add("active");
    btn.innerHTML = "✅ Trail: ON";
    setTrailPref(id, true);
  }
};

// V3.4.0: persist which cats have their trail shown, so the choice
// survives a reload. Stored as an array of ids under prefs key 'trails'.
function setTrailPref(id, on) {
  if (!window.Prefs) return;
  const set = new Set(window.Prefs.get("trails", []));
  if (on) set.add(id);
  else set.delete(id);
  window.Prefs.set("trails", Array.from(set));
}

// V3.4.0: re-apply view prefs that need markers/cards to already exist.
// Called from hydrateFromServer() once /data has rendered the cats.
window.applyMarkerDependentPrefs = function () {
  if (!window.Prefs) return;

  // Re-enable trails the user had switched on.
  const trails = window.Prefs.get("trails", []);
  if (Array.isArray(trails)) {
    trails.forEach((id) => {
      const btn = document.getElementById(`breadcrumb-btn-${id}`);
      if (btn && !btn.classList.contains("active")) {
        showBreadcrumbs(id);
        btn.classList.add("active");
        btn.innerHTML = "✅ Trail: ON";
      }
    });
  }

  // Re-apply the follow target.
  const follow = window.Prefs.get("follow", null);
  if (follow && markers[follow]) {
    followedMarkerId = follow;
    const btn = document.getElementById(`follow-btn-${follow}`);
    if (btn) {
      btn.classList.add("active");
      btn.innerHTML = "✅ Following";
    }
  }
};

// Toggle console log
window.toggleConsoleCard = function (id) {
  const console = document.getElementById(`console-${id}`);
  console.classList.toggle("show");
};

// Toggle follow marker - only one can be active at a time
window.toggleFollowMarker = function (id) {
  const btn = document.getElementById(`follow-btn-${id}`);
  const isFollowing = followedMarkerId === id;

  if (isFollowing) {
    // Turn off follow for this marker
    followedMarkerId = null;
    btn.classList.remove("active");
    btn.innerHTML = "📌 Follow: OFF";
  } else {
    // Turn off any previously followed marker
    if (followedMarkerId) {
      const oldBtn = document.getElementById(`follow-btn-${followedMarkerId}`);
      if (oldBtn) {
        oldBtn.classList.remove("active");
        oldBtn.innerHTML = "📌 Follow: OFF";
      }
    }

    // Turn on follow for this marker
    followedMarkerId = id;
    btn.classList.add("active");
    btn.innerHTML = "✅ Following";

    // Center map on this marker immediately
    if (markers[id]) {
      const latlng = markers[id].getLatLng();
      map.setView(latlng, 16);
    }
  }
  // V3.4.0: remember the follow target across reloads.
  if (window.Prefs) window.Prefs.set("follow", followedMarkerId);
}; // Update marker card with new data
function updateMarkerCard(id, status, data) {
  const card = document.getElementById(`marker-card-${id}`);
  if (!card) return;

  // Apply sheen effect to indicate update
  // Remove any existing sheen animation
  card.classList.remove("card-sheen");
  // Force reflow to restart animation
  void card.offsetWidth;
  // Add sheen animation class
  card.classList.add("card-sheen");
  // Remove class after animation completes
  setTimeout(() => {
    card.classList.remove("card-sheen");
  }, 800);

  // Update status class. Sanitise status to a known token so a malicious
  // status from the wire can't break our class names either.
  const safeStatus = String(status).toLowerCase().replace(/[^a-z]/g, '');
  card.className = `marker-card status-${safeStatus}`;
  // Re-add sheen class (since we just overwrote className)
  card.classList.add("card-sheen");

  // Update status text — textContent is safe (no HTML parsing)
  const statusEl = document.getElementById(`card-status-${id}`);
  if (statusEl) {
    statusEl.textContent = status;
  }

  // Update icon. V3.6.0: `id` is the device_id (UID) key; icon files are
  // named by the friendly NAME, so resolve the label first.
  const name = (window.deviceNames && window.deviceNames[id]) || id;
  const iconEl = document.getElementById(`card-icon-${id}`);
  if (iconEl) {
    if (KNOWN_CATS.includes(name)) {
      let iconStatus;
      switch (status) {
        case "Home":    iconStatus = "Home"; break;
        case "Roaming": iconStatus = "Outanabout"; break; // V3.1.9: rename
        case "Offline": iconStatus = "Offline"; break;
        case "Error":   iconStatus = "Error"; break;
        default:        iconStatus = "Error";
      }
      iconEl.src = `/icons/${name}_Marker_${iconStatus}.avif`;
    } else {
      // V3.1.7: unknown collar — inline SVG dot, no broken-image link
      const colour = statusColour(status);
      const svg = `<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>` +
                  `<circle cx='12' cy='12' r='10' fill='${colour}' stroke='white' stroke-width='2'/>` +
                  `</svg>`;
      iconEl.src = "data:image/svg+xml;utf8," + encodeURIComponent(svg);
    }
  }

  // Add message to history (keep last 5)
  if (!markerHistory[id]) {
    markerHistory[id] = [];
  }
  const now = new Date();
  const timeStr = now.toLocaleTimeString();
  markerHistory[id].unshift({
    time: timeStr,
    data: data,
  });
  if (markerHistory[id].length > 5) {
    markerHistory[id].pop();
  }

  // Update console messages
  const consoleMessages = document.getElementById(`console-messages-${id}`);
  if (consoleMessages) {
    consoleMessages.innerHTML = markerHistory[id]
      .map(
        (msg) => `
      <div class="console-message">
        <span class="console-time">${msg.time}</span><br>
        <pre style="margin: 0; white-space: pre-wrap; word-break: break-all;">${JSON.stringify(
          msg.data,
          null,
          2
        )}</pre>
      </div>
    `
      )
      .join("");
  }
}

// Helper functions for breadcrumbs
function showBreadcrumbs(id) {
  if (!window.breadcrumbs[id] || window.breadcrumbs[id].length < 2) return;

  if (window.breadcrumbLines[id]) {
    map.removeLayer(window.breadcrumbLines[id]);
  }

  window.breadcrumbLines[id] = L.polyline(window.breadcrumbs[id], {
    color: getBreadcrumbColor(id),
    weight: 3,
    dashArray: "10, 10",
    opacity: 0.7,
  }).addTo(map);
}

function hideBreadcrumbs(id) {
  if (window.breadcrumbLines[id]) {
    map.removeLayer(window.breadcrumbLines[id]);
    window.breadcrumbLines[id] = null;
  }
}

// WebSocket + BLE helpers
window.ws = null; // Make globally accessible
let debugMessageCount = 0;

// Global message log - expose globally for debug console access
window.globalMessageLog = [];
const globalMessageLog = window.globalMessageLog; // Local reference
const MAX_GLOBAL_LOG_ENTRIES = 50; // Keep last 50 messages

function handleBleState(data) {
  if (!data || data.type !== "ble_state") return;
  const bleToggle = document.getElementById("bleToggle");
  const bleStatus = document.getElementById("bleStatus");
  const isOn = !!data.on;
  if (bleToggle) bleToggle.checked = isOn;
  if (bleStatus) {
    bleStatus.textContent = isOn ? "ON" : "OFF";
    bleStatus.style.color = isOn ? "#28a745" : "#dc3545";
  }
}

function requestBleStatus() {
  try {
    if (window.ws && window.ws.readyState === 1) {
      console.log("BLE get via WS");
      window.ws.send(JSON.stringify({ type: "ble_get" }));
    } else {
      console.log("WS not ready for BLE get");
    }
  } catch (e) {
    console.warn("BLE get failed:", e);
  }
}

// expose
window.requestBleStatus = requestBleStatus;

window.sendBle = function (turnOn) {
  const payload = { type: "ble_set", on: !!turnOn };
  if (window.ws && window.ws.readyState === 1) {
    console.log("BLE set via WS:", payload);
    window.ws.send(JSON.stringify(payload));
    return;
  }
  console.warn("WS not ready to send BLE set");
};

// V3.6.8: developer-mode helpers (same shape as the BLE ones). dev_state is
// pushed by the receiver; dev_get/dev_set request + change it.
function handleDevState(data) {
  if (!data || data.type !== "dev_state") return;
  const devToggle = document.getElementById("devToggle");
  const devStatus = document.getElementById("devStatus");
  const isOn = !!data.on;
  if (devToggle) devToggle.checked = isOn;
  if (devStatus) {
    devStatus.textContent = isOn ? "ON" : "OFF";
    devStatus.style.color = isOn ? "#ffc107" : "#6c757d";
  }
}

function requestDevStatus() {
  try {
    if (window.ws && window.ws.readyState === 1) {
      window.ws.send(JSON.stringify({ type: "dev_get" }));
    }
  } catch (e) {
    console.warn("dev get failed:", e);
  }
}
window.requestDevStatus = requestDevStatus;

window.sendDev = function (on) {
  const payload = { type: "dev_set", on: !!on };
  if (window.ws && window.ws.readyState === 1) {
    console.log("Developer mode set via WS:", payload);
    window.ws.send(JSON.stringify(payload));
    return;
  }
  console.warn("WS not ready to send dev_set");
};

function connectWebSocket() {
  window.ws = new WebSocket(`ws://${window.location.hostname}:81/`);

  window.ws.onopen = () => {
    console.log("WebSocket connected successfully to ESP32");
    console.log("WebSocket readyState:", window.ws.readyState);

    // Update connection status immediately
    if (window.updateStatus) {
      window.updateStatus("WiFi/Network Connected: ✔", "green");
    }

    requestBleStatus();
    requestDevStatus(); // V3.6.8: sync the developer-mode toggle on (re)connect

    // V3.4.0: rebuild the map from the receiver's stored state on every
    // (re)connect — covers initial page load, refresh, and reconnect.
    if (window.hydrateFromServer) window.hydrateFromServer();
  };

  window.ws.onerror = (error) => {
    console.error("WebSocket error:", error);
    console.log("WebSocket readyState:", window.ws.readyState);

    // Update connection status immediately
    if (window.updateStatus) {
      window.updateStatus("WebSocket Error❌", "red");
    }
  };

  window.ws.onclose = () => {
    console.log("WebSocket closed. Attempting to reconnect...");
    console.log("WebSocket readyState:", window.ws.readyState);

    // Update connection status immediately
    if (window.updateStatus) {
      window.updateStatus("Disconnected (Reconnecting...)❌", "red");
    }

    setTimeout(connectWebSocket, 2000); // Reconnect after 2 seconds
  };

  window.ws.onmessage = (event) => {
    console.log("WebSocket message received:", event.data);

    // Add to global message log with parsed data
    const timestamp = new Date().toLocaleTimeString();
    let parsedData;
    try {
      parsedData = JSON.parse(event.data);
    } catch (e) {
      parsedData = event.data;
    }

    globalMessageLog.unshift({
      timestamp: timestamp,
      data: parsedData,
    });

    // Keep only last MAX_GLOBAL_LOG_ENTRIES
    if (globalMessageLog.length > MAX_GLOBAL_LOG_ENTRIES) {
      globalMessageLog.pop();
    }

    // Update debug console if function exists
    if (window.updateDebugConsole) {
      window.updateDebugConsole();
    }

    try {
      const data = JSON.parse(event.data);
      window.handleTelemetry(data, false);
    } catch (error) {
      console.error("Error processing WebSocket message:", error);
    }
  };
}

// V3.4.0: shared telemetry/message handler. Called LIVE from ws.onmessage
// (isHydrate=false) and once per cat from hydrateFromServer() on page load
// / WS reconnect (isHydrate=true). Centralising the render path is what
// lets a reloaded page (or a brand-new client) rebuild the entire map from
// the receiver's RAM via /data, instead of waiting for the next live packet.
window.handleTelemetry = function (data, isHydrate) {
    try {
      // Handle BLE state response
      if (data.type === "ble_state") {
        console.log("BLE state update received:", data);
        handleBleState(data);
        return; // Don't process as position data
      }

      // V3.6.8: developer-mode state push
      if (data.type === "dev_state") {
        handleDevState(data);
        return;
      }

      // V3: dynamic home location updated from server.
      if (data.type === "home_location" && typeof data.lat === "number" && typeof data.lon === "number") {
        HOME_LOCATION = [data.lat, data.lon];
        console.log("Home location updated via WS:", HOME_LOCATION);
        if (window.onHomeLocationUpdated) {
          window.onHomeLocationUpdated(data.lat, data.lon);
        }
        return;
      }

      // V3.1.4: command status updates pushed every time a queued command
      // changes state on the receiver. The handler is defined in
      // index.html's inline JS — we just dispatch.
      if (data.type === "command_status" && window.applyCommandUpdate) {
        window.applyCommandUpdate(data);
        return;
      }

      // V3.9.0: the base pushes a presence message when a collar's wake-up
      // packet arrives. Mark that collar "awake" for 60 s so the marker tile
      // shows it's reachable for OTAP commands right now.
      if (data.type === "presence" && data.device_id !== undefined && window.markCollarAwake) {
        window.markCollarAwake(String(data.device_id));
        return;
      }

      // Handle node state updates (Command & Control)
      if (data.type === "node_states" || data.type === "node_alert") {
        if (window.handleNodeStateUpdate) {
          window.handleNodeStateUpdate(data);
        }
        // Don't return - position data may also be present
      }

      // V3.6.0: the marker / state / DOM KEY is the immutable device_id
      // (UID). The friendly name is a mutable display label, kept in
      // window.deviceNames[key] and used for popups, card titles, and
      // icon-file selection. MyDevice (the receiver's own GPS) has no
      // device_id, so it keeps the literal "MyDevice" key.
      // V3.9.0: the base station is reserved ID 1 (BASE_ID) and now reports its
      // own GPS with device_id:1. Treat that (or the legacy id:"MyDevice") as
      // the receiver's self-marker, keeping the internal "MyDevice" key so every
      // existing MyDevice special-case (icon, follow, distance reference, no
      // timeout) keeps working unchanged.
      const isMyDevice = (data.id === "MyDevice") || (data.device_id === 1);
      const hasUid = (data.device_id !== undefined && data.device_id !== null) && !isMyDevice;
      if (hasUid || isMyDevice) {
        const id = isMyDevice ? "MyDevice" : String(data.device_id);
        const name = isMyDevice ? "MyDevice" : (data.name || ("Device-" + data.device_id));
        // V3.6.2: funnel through the shared setter so the marker-tile title
        // and the deviceNames map update consistently with the node-state path.
        if (window.setDeviceName) window.setDeviceName(id, name);
        else window.deviceNames[id] = name;
        const status = data.status || "Unknown";

        // Update last received time for this marker
        markerLastUpdate[id] = Date.now();

        // Create marker card if it doesn't exist
        createMarkerCard(id, status);

        // Create or update marker
        if (!markers[id]) {
          console.log(`Creating new marker for ${id} at HOME_LOCATION`);
          markers[id] = L.marker(HOME_LOCATION, {
            icon: getMarkerIcon(id, status),
          }).addTo(map);
          markerVisibility[id] = true;
        }

        // Always update marker icon based on status (even without coordinates)
        if (markers[id]) {
          markers[id].setIcon(getMarkerIcon(id, status));

          // Animate marker - grow 50% and return to normal over 1 second
          // Using a separate animation layer to avoid interfering with Leaflet positioning
          const markerElement = markers[id].getElement();
          if (markerElement) {
            // Remove any existing animation class first
            markerElement.classList.remove("marker-pulse");
            // Force reflow to restart animation
            void markerElement.offsetWidth;
            // Add animation class
            markerElement.classList.add("marker-pulse");
            // Remove class after animation completes
            setTimeout(() => {
              markerElement.classList.remove("marker-pulse");
            }, 600);
          }
        }

        // Update position if we have coordinates
        if (data.lat && data.lon) {
          console.log(`Updating ${id} position to:`, data.lat, data.lon);
          const newPos = [data.lat, data.lon];

          // Use setLatLng without animation to avoid rendering issues
          markers[id].setLatLng(newPos);

          // Force map to recalculate marker positions
          markers[id]._updateZIndex();

          // Update breadcrumbs trail. V3.4.0: on hydrate, seed the whole
          // trail from the server's history (data.trail = [[lat,lon],…]) so
          // the line redraws complete after a reload. Live packets append a
          // single point. Cap honours the user's trail-length pref.
          const maxPts = window.MAX_TRAIL_POINTS || 4;
          if (isHydrate && Array.isArray(data.trail) && data.trail.length) {
            window.breadcrumbs[id] = data.trail
              .filter((p) => Array.isArray(p) && p.length >= 2)
              .map((p) => [p[0], p[1]]);
          } else {
            if (!window.breadcrumbs[id]) {
              window.breadcrumbs[id] = [];
            }
            window.breadcrumbs[id].push(newPos);
          }
          while (window.breadcrumbs[id].length > maxPts) {
            window.breadcrumbs[id].shift();
          }

          // Update breadcrumb line if it's enabled
          const btn = document.getElementById(`breadcrumb-btn-${id}`);
          if (btn && btn.classList.contains("active")) {
            showBreadcrumbs(id);
          }

          // Update marker popup with hover behavior
          // Calculate distance from MyDevice if available, otherwise use HOME_LOCATION
          const referencePos = markers["MyDevice"]
            ? markers["MyDevice"].getLatLng()
            : HOME_LOCATION;
          const distance = (map.distance(newPos, referencePos) / 1000).toFixed(
            2
          );
          markers[id].unbindPopup(); // Remove any existing popup
          markers[id].bindPopup(`
            <h5>${name}</h5>
            ${id !== "MyDevice" ? `<p><strong>Device ID:</strong> ${id}</p>` : ""}
            <p><strong>Status:</strong> ${status}</p>
            <p><strong>Coordinates:</strong> ${data.lat.toFixed(
              6
            )}, ${data.lon.toFixed(6)}</p>
            <p><strong>Distance from home:</strong> ${distance} km</p>
          `);

          // Add hover events for popup
          markers[id].off("mouseover").on("mouseover", function () {
            this.openPopup();
          });
          markers[id].off("mouseout").on("mouseout", function () {
            this.closePopup();
          });

          // Auto-center map if this marker is being followed
          if (followedMarkerId === id) {
            map.setView(newPos, 16);
          }

          console.log(`Position updated for ${id}:`, data.lat, data.lon);
        }

        // Update marker card
        updateMarkerCard(id, status, data);
      }
    } catch (error) {
      console.error("[handleTelemetry] error:", error);
    }
};

// V3.4.0: pull the receiver's last-known state on load / reconnect and
// render it immediately, so a refresh or a new client never starts blank.
// /data → array of cat payloads (each may carry a `trail` history array);
// /node-states → array of node modes for the C&C panel + marker cards.
window.hydrateFromServer = function () {
  fetch("/data", { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : []))
    .then((arr) => {
      if (Array.isArray(arr)) {
        arr.forEach((item) => {
          if (item && item.id) window.handleTelemetry(item, true);
        });
        console.log(`[hydrate] restored ${arr.length} cat(s) from /data`);
        // Re-apply persisted view prefs that depend on markers existing.
        if (window.applyMarkerDependentPrefs) window.applyMarkerDependentPrefs();
      }
    })
    .catch((e) => console.warn("[hydrate] /data fetch failed:", e));

  fetch("/node-states", { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : null))
    .then((nodes) => {
      if (Array.isArray(nodes) && window.handleNodeStateUpdate) {
        window.handleNodeStateUpdate({ type: "node_states", nodes: nodes });
        console.log(`[hydrate] restored ${nodes.length} node(s) from /node-states`);
      }
    })
    .catch((e) => console.warn("[hydrate] /node-states fetch failed:", e));
};

// Old updateGlobalConsole function removed - now handled in index.html

// Function to check for markers that haven't been updated in 10 minutes
function checkMarkerTimeouts() {
  const now = Date.now();
  const TIMEOUT_THRESHOLD = 10 * 60 * 1000; // 10 minutes in milliseconds

  Object.keys(markers).forEach((id) => {
    // Don't timeout MyDevice (the receiver's own GPS)
    if (id === "MyDevice") return;

    const lastUpdate = markerLastUpdate[id];
    if (lastUpdate && now - lastUpdate > TIMEOUT_THRESHOLD) {
      // Update to Offline status if not already offline
      const card = document.getElementById(`dropdown-${id}`);
      if (card && markers[id]) {
        // Get current status from card to avoid redundant updates
        const statusElement = card.querySelector(".status");
        const currentStatus = statusElement
          ? statusElement.textContent.trim()
          : "";

        if (!currentStatus.includes("Offline")) {
          updateCard(id, "Offline");
          markers[id].setIcon(getMarkerIcon(id, "Offline"));
          console.log(
            `[TIMEOUT] Marker ${id} set to Offline (no update for 10 minutes)`
          );
        }
      }
    }
  });
}

// Start WebSocket connection
connectWebSocket();

// Check for timed-out markers every 30 seconds
setInterval(checkMarkerTimeouts, 30000);

// Create MyDevice marker card immediately on page load
createMarkerCard("MyDevice", "Starting up");

// Enable follow for MyDevice by default after a short delay (to ensure button exists)
setTimeout(() => {
  if (window.toggleFollowMarker) {
    window.toggleFollowMarker("MyDevice");
  }
}, 100);

console.log("Minimal Leaflet2.js loaded successfully");

// ===== 3. MEASURE TOOL (Distance/Area) =====
let measurePath = null;
let measureMarkers = [];
let isMeasuring = false;

const measureControl = L.control({ position: "topleft" });
measureControl.onAdd = function (map) {
  const div = L.DomUtil.create("div", "leaflet-bar leaflet-control");
  div.innerHTML =
    '<a href="#" title="Measure distance (click to start/stop)" style="width:30px;height:30px;line-height:30px;text-align:center;text-decoration:none;font-size:18px;">📏</a>';

  L.DomEvent.on(div, "click", function (e) {
    L.DomEvent.stopPropagation(e);
    L.DomEvent.preventDefault(e);

    if (!isMeasuring) {
      // Start measuring
      isMeasuring = true;
      div.style.backgroundColor = "#ffc107";
      div.title = "Click map to add points, click here to finish";
      measurePath = L.polyline([], {
        color: "#ff0000",
        weight: 3,
        dashArray: "10, 10",
      }).addTo(map);

      map.on("click", addMeasurePoint);
      map.getContainer().style.cursor = "crosshair";
    } else {
      // Stop measuring
      stopMeasuring();
      div.style.backgroundColor = "";
      div.title = "Measure distance (click to start/stop)";
    }
  });

  return div;
};
measureControl.addTo(map);

function addMeasurePoint(e) {
  const latlng = e.latlng;

  // Add point to path
  const latlngs = measurePath.getLatLngs();
  latlngs.push(latlng);
  measurePath.setLatLngs(latlngs);

  // Add marker
  const marker = L.circleMarker(latlng, {
    radius: 5,
    color: "#ff0000",
    fillColor: "#ffffff",
    fillOpacity: 1,
    weight: 2,
  })
    .addTo(map)
    .bindPopup(`<b>Start Point</b><br><small>Click to add more points</small>`)
    .openPopup();
  measureMarkers.push(marker);

  // Calculate and show distance
  if (latlngs.length > 1) {
    let totalDistance = 0;
    for (let i = 1; i < latlngs.length; i++) {
      totalDistance += latlngs[i - 1].distanceTo(latlngs[i]);
    }

    const distanceText =
      totalDistance < 1000
        ? `${totalDistance.toFixed(1)} m`
        : `${(totalDistance / 1000).toFixed(2)} km`;

    marker
      .bindPopup(
        `<b>Total Distance:</b> ${distanceText}<br><small>Point ${latlngs.length}</small>`
      )
      .openPopup();
  }
}

function stopMeasuring() {
  isMeasuring = false;
  map.off("click", addMeasurePoint);
  map.getContainer().style.cursor = "";

  // Clear measuring elements
  if (measurePath) {
    map.removeLayer(measurePath);
    measurePath = null;
  }
  measureMarkers.forEach((m) => map.removeLayer(m));
  measureMarkers = [];
}

// ===== 6. SIMPLE MINIMAP (No plugin required) =====
const minimapControl = L.control({ position: "bottomleft" });
minimapControl.onAdd = function (mainMap) {
  const container = L.DomUtil.create(
    "div",
    "leaflet-control minimap-container"
  );
  container.style.cssText =
    "width:150px;height:150px;border:2px solid rgba(0,0,0,0.2);border-radius:4px;background:white;box-shadow:0 1px 5px rgba(0,0,0,0.4);cursor:pointer;overflow:hidden;";

  // Create minimap
  const minimap = L.map(container, {
    center: mainMap.getCenter(),
    zoom: mainMap.getZoom() - 5,
    dragging: false,
    doubleClickZoom: false,
    scrollWheelZoom: false,
    attributionControl: false,
    zoomControl: false,
  });

  // Add tile layer to minimap
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    minZoom: 0,
    maxZoom: 13,
  }).addTo(minimap);

  // Add rectangle showing main map view
  const viewRect = L.rectangle(mainMap.getBounds(), {
    color: "#ff7800",
    weight: 2,
    fillOpacity: 0.1,
  }).addTo(minimap);

  // Update minimap when main map moves
  mainMap.on("moveend", function () {
    minimap.setView(mainMap.getCenter(), mainMap.getZoom() - 5);
    viewRect.setBounds(mainMap.getBounds());
  });

  // Click minimap to center main map
  L.DomEvent.on(container, "click", function (e) {
    L.DomEvent.stopPropagation(e);
    const bounds = mainMap.getBounds();
    const center = mainMap.getCenter();
    // Optional: Add interaction here
  });

  // Prevent map interactions from bubbling to main map
  L.DomEvent.disableClickPropagation(container);
  L.DomEvent.disableScrollPropagation(container);

  return container;
};
minimapControl.addTo(map);

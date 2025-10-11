// Minimal Leaflet2.js - Just basic map functionality
const HOME_LOCATION = [51.87378215701798, -2.239428653198173];

// Initialize the map with minimal configuration
const map = L.map("map", {
  center: HOME_LOCATION,
  zoom: 19,
  maxZoom: 23,
});

// Use only OpenStreetMap - no providers needed
const osm = L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
  maxZoom: 19,
  attribution:
    '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
}).addTo(map);

// Define KNOWN_CATS
const KNOWN_CATS = ["Podge", "Macy", "Gizmo", "Simba"];

// Essential variables
const dropdowns = new Set();
const markers = {};
const markerVisibility = {};
const autoCenter = {};
// Provide globals used by index.html helpers
window.breadcrumbs = window.breadcrumbs || {};
window.breadcrumbLines = window.breadcrumbLines || {};

// Home marker
const HOME_ICON = L.icon({
  iconUrl: "/icons/Home.png",
  iconSize: [32, 32],
  iconAnchor: [16, 32],
  popupAnchor: [0, -32],
});

L.marker(HOME_LOCATION, {
  icon: HOME_ICON,
})
  .addTo(map)
  .bindPopup("Home Base")
  .openPopup();

// Basic breadcrumb function
// Basic marker icon function
function getMarkerIcon(id, status) {
  const baseId = KNOWN_CATS.includes(id) ? id : "generic";
  let iconStatus;

  // Map to simplified 4-state system
  switch (status) {
    case "Home":
      iconStatus = "Home";
      break;
    case "Out":
      iconStatus = "Outanabout"; // Keep existing icon filename
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
    iconUrl: `/icons/${baseId}_Marker_${iconStatus}.png`,
    iconSize: [32, 32],
    iconAnchor: [16, 32],
    popupAnchor: [0, -32],
  });
}

// Basic dropdown creation function
function createCatDropdown(id, iconUrl, status) {
  if (dropdowns.has(id)) return;

  console.log(`Creating dropdown for ${id} with status ${status}`);
  dropdowns.add(id);

  // This would normally create the HTML dropdown
  // For now, just log it
  console.log(`Cat ${id} would appear here with status ${status}`);
}

// WebSocket + BLE helpers
window.ws = null; // Make globally accessible
let debugMessageCount = 0;

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

function connectWebSocket() {
  window.ws = new WebSocket(`ws://${window.location.hostname}:81/`);

  window.ws.onopen = () => {
    console.log("WebSocket connected successfully to ESP32");
    console.log("WebSocket readyState:", window.ws.readyState);
    requestBleStatus();
  };

  window.ws.onmessage = (event) => {
    console.log("WebSocket message received:", event.data);

    try {
      const data = JSON.parse(event.data);
      console.log("Parsed data:", data);

      // Handle BLE state response
      if (data.type === "ble_state") {
        console.log("BLE state update received:", data);
        handleBleState(data);
        return; // Don't process as position data
      }

      if (data.id) {
        const id = data.id;

        // Create dropdown
        createCatDropdown(id, "", data.status || "Unknown");

        // Create or update marker
        if (!markers[id]) {
          console.log(`Creating new marker for ${id}`);
          markers[id] = L.marker([0, 0], {
            icon: getMarkerIcon(id, data.status),
          }).addTo(map);
          markerVisibility[id] = true;
        }

        // Update position if we have coordinates
        if (data.lat && data.lon) {
          console.log(`Updating ${id} position to:`, data.lat, data.lon);
          const newPos = [data.lat, data.lon];
          markers[id].setLatLng(newPos);

          // Update marker popup
          markers[id].bindPopup(`
            <h5>${id}</h5>
            <p><strong>Status:</strong> ${data.status || "Unknown"}</p>
            <p><strong>Coordinates:</strong> ${data.lat.toFixed(
              6
            )}, ${data.lon.toFixed(6)}</p>
          `);

          // Skip breadcrumbs in minimal version
          console.log(`Position updated for ${id}:`, data.lat, data.lon);
        }
      }
    } catch (error) {
      console.error("Error processing WebSocket message:", error);
    }
  };

  window.ws.onclose = () => {
    console.log("WebSocket closed. Attempting to reconnect...");
    console.log("WebSocket readyState:", window.ws.readyState);
    setTimeout(connectWebSocket, 5000);
  };

  window.ws.onerror = (error) => {
    console.error("WebSocket error:", error);
    console.log("WebSocket readyState:", window.ws.readyState);
  };
}

// Start WebSocket connection
connectWebSocket();

console.log("Minimal Leaflet2.js loaded successfully");

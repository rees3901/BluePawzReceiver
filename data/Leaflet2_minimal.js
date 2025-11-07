// Minimal Leaflet2.js - Just basic map functionality
const HOME_LOCATION = [51.87378215701798, -2.239428653198173];

// Initialize the map with minimal configuration
const map = L.map("map", {
  center: HOME_LOCATION,
  zoom: 19,
  maxZoom: 23,
});

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

// Create layer control with all map options
const baseMaps = {
  "Street Map": osmLayer,
  Satellite: satelliteLayer,
  Topographic: topoLayer,
};

// Add layer control to map (top-right corner)
L.control
  .layers(baseMaps, null, {
    position: "topright",
    collapsed: false, // Set to true to collapse the control
  })
  .addTo(map);

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

// Store message history for each marker
const markerHistory = {};

// Create marker card dynamically
function createMarkerCard(id, status) {
  if (dropdowns.has(id)) return;

  console.log(`Creating marker card for ${id} with status ${status}`);
  dropdowns.add(id);

  // Initialize message history
  if (!markerHistory[id]) {
    markerHistory[id] = [];
  }

  const baseId = KNOWN_CATS.includes(id) ? id : "generic";
  const iconUrl = `/icons/${baseId}_Marker_Home.png`;

  const statusClass = status
    ? `status-${status.toLowerCase()}`
    : "status-offline";

  const card = document.createElement("div");
  card.className = `marker-card ${statusClass}`;
  card.id = `marker-card-${id}`;

  card.innerHTML = `
    <div class="marker-card-header">
      <img src="${iconUrl}" alt="${id}" class="marker-card-icon" id="card-icon-${id}">
      <div class="marker-card-title">
        <h3 class="marker-card-name">${id}</h3>
        <p class="marker-card-status" id="card-status-${id}">${
    status || "Unknown"
  }</p>
      </div>
    </div>
    <div class="marker-card-actions">
      <button class="marker-card-btn btn-jump" onclick="jumpToMarker('${id}')">
        🎯 Jump to Location
      </button>
      <button class="marker-card-btn btn-breadcrumb" id="breadcrumb-btn-${id}" onclick="toggleBreadcrumbsCard('${id}')">
        📍 Trail: OFF
      </button>
      <button class="marker-card-btn btn-console" onclick="toggleConsoleCard('${id}')">
        📄 Message Log
      </button>
    </div>
    <div class="marker-card-console" id="console-${id}">
      <div class="console-messages" id="console-messages-${id}">
        No messages yet
      </div>
    </div>
  `;

  document.querySelector(".marker-cards-container").appendChild(card);
}

// Jump to marker location
window.jumpToMarker = function (id) {
  if (markers[id]) {
    const latlng = markers[id].getLatLng();
    map.setView(latlng, 18);
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
  } else {
    showBreadcrumbs(id);
    btn.classList.add("active");
    btn.innerHTML = "✅ Trail: ON";
  }
};

// Toggle console log
window.toggleConsoleCard = function (id) {
  const console = document.getElementById(`console-${id}`);
  console.classList.toggle("show");
};

// Update marker card with new data
function updateMarkerCard(id, status, data) {
  const card = document.getElementById(`marker-card-${id}`);
  if (!card) return;

  // Update status class
  card.className = `marker-card status-${status.toLowerCase()}`;

  // Update status text
  const statusEl = document.getElementById(`card-status-${id}`);
  if (statusEl) {
    statusEl.textContent = status;
  }

  // Update icon
  const baseId = KNOWN_CATS.includes(id) ? id : "generic";
  let iconStatus;
  switch (status) {
    case "Home":
      iconStatus = "Home";
      break;
    case "Out":
      iconStatus = "Outanabout";
      break;
    case "Offline":
      iconStatus = "Offline";
      break;
    case "Error":
      iconStatus = "Error";
      break;
    default:
      iconStatus = "Error";
  }
  const iconEl = document.getElementById(`card-icon-${id}`);
  if (iconEl) {
    iconEl.src = `/icons/${baseId}_Marker_${iconStatus}.png`;
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
    color: "#007bff",
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
        const status = data.status || "Unknown";

        // Create marker card if it doesn't exist
        createMarkerCard(id, status);

        // Create or update marker
        if (!markers[id]) {
          console.log(`Creating new marker for ${id}`);
          markers[id] = L.marker([0, 0], {
            icon: getMarkerIcon(id, status),
          }).addTo(map);
          markerVisibility[id] = true;
        }

        // Update position if we have coordinates
        if (data.lat && data.lon) {
          console.log(`Updating ${id} position to:`, data.lat, data.lon);
          const newPos = [data.lat, data.lon];
          markers[id].setLatLng(newPos);

          // Update marker icon based on status
          markers[id].setIcon(getMarkerIcon(id, status));

          // Update breadcrumbs trail (keep last 3 positions)
          if (!window.breadcrumbs[id]) {
            window.breadcrumbs[id] = [];
          }
          window.breadcrumbs[id].push(newPos);
          if (window.breadcrumbs[id].length > 3) {
            window.breadcrumbs[id].shift();
          }

          // Update breadcrumb line if it's enabled
          const btn = document.getElementById(`breadcrumb-btn-${id}`);
          if (btn && btn.classList.contains("active")) {
            showBreadcrumbs(id);
          }

          // Update marker popup
          const distance = (map.distance(newPos, HOME_LOCATION) / 1000).toFixed(
            2
          );
          markers[id].bindPopup(`
            <h5>${id}</h5>
            <p><strong>Status:</strong> ${status}</p>
            <p><strong>Coordinates:</strong> ${data.lat.toFixed(
              6
            )}, ${data.lon.toFixed(6)}</p>
            <p><strong>Distance from home:</strong> ${distance} km</p>
          `);

          console.log(`Position updated for ${id}:`, data.lat, data.lon);
        }

        // Update marker card
        updateMarkerCard(id, status, data);
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

// ===== 3. LOCATE ME CONTROL (Geolocation) =====
const locateControl = L.control({ position: "topleft" });
locateControl.onAdd = function (map) {
  const div = L.DomUtil.create("div", "leaflet-bar leaflet-control");
  div.innerHTML =
    '<a href="#" title="Find my location" style="width:30px;height:30px;line-height:30px;text-align:center;text-decoration:none;font-size:18px;">📍</a>';

  L.DomEvent.on(div, "click", function (e) {
    L.DomEvent.stopPropagation(e);
    L.DomEvent.preventDefault(e);

    if (navigator.geolocation) {
      div.innerHTML =
        '<a href="#" style="width:30px;height:30px;line-height:30px;text-align:center;text-decoration:none;font-size:18px;">⏳</a>';

      navigator.geolocation.getCurrentPosition(
        function (position) {
          div.innerHTML =
            '<a href="#" title="Find my location" style="width:30px;height:30px;line-height:30px;text-align:center;text-decoration:none;font-size:18px;">📍</a>';

          const lat = position.coords.latitude;
          const lng = position.coords.longitude;
          const accuracy = position.coords.accuracy;

          // Remove old location marker if exists
          if (window.userLocationMarker) {
            map.removeLayer(window.userLocationMarker);
            if (window.userAccuracyCircle) {
              map.removeLayer(window.userAccuracyCircle);
            }
          }

          // Add accuracy circle
          window.userAccuracyCircle = L.circle([lat, lng], {
            radius: accuracy,
            color: "#3388ff",
            fillColor: "#3388ff",
            fillOpacity: 0.15,
            weight: 2,
          }).addTo(map);

          // Add marker for your location
          window.userLocationMarker = L.marker([lat, lng], {
            icon: L.icon({
              iconUrl:
                "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIyNCIgaGVpZ2h0PSIyNCIgdmlld0JveD0iMCAwIDI0IDI0Ij48Y2lyY2xlIGN4PSIxMiIgY3k9IjEyIiByPSI4IiBmaWxsPSIjMzM4OGZmIiBzdHJva2U9IndoaXRlIiBzdHJva2Utd2lkdGg9IjIiLz48Y2lyY2xlIGN4PSIxMiIgY3k9IjEyIiByPSIzIiBmaWxsPSJ3aGl0ZSIvPjwvc3ZnPg==",
              iconSize: [24, 24],
              iconAnchor: [12, 12],
            }),
          })
            .addTo(map)
            .bindPopup(
              `<b>Your Location</b><br>Accuracy: ±${accuracy.toFixed(0)}m`
            )
            .openPopup();
          map.setView([lat, lng], 16);
        },
        function (error) {
          div.innerHTML =
            '<a href="#" title="Find my location" style="width:30px;height:30px;line-height:30px;text-align:center;text-decoration:none;font-size:18px;">📍</a>';
          alert("Unable to get your location: " + error.message);
        }
      );
    } else {
      alert("Geolocation is not supported by your browser");
    }
  });

  return div;
};
locateControl.addTo(map);

// ===== 4. MEASURE TOOL (Distance/Area) =====
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

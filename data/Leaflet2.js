// Define HOME_LOCATION at the top of the file
const HOME_LOCATION = [51.87378215701798, -2.239428653198173];

// Initialize the map
const map = L.map("map", {
  center: HOME_LOCATION, // Center the map at the home location
  zoom: 19, // Adjusted zoom level for better visibility
  maxZoom: 23, // Allow zooming in further
  rotate: true, // Ensure rotate plugin is properly configured
  rotateControl: {
    position: "topleft",
    closeOnZeroBearing: false,
    bearingText: "°",
  },
});

// Add OpenStreetMap tile layer
L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
  attribution:
    '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
}).addTo(map);

// Define KNOWN_CATS to avoid ReferenceError
const KNOWN_CATS = [
  "Podge",
  "Macy",
  "Gizmo",
  "Simba",
  "Carrie",
  "Chloe",
  "MyDevice",
];

// Ensure dropdowns is defined
const dropdowns = new Set(); // Initialize dropdowns as a Set

// Initialize markers object
const markers = {}; // Store markers by ID

// Initialize tracking objects
const markerVisibility = {}; // ID → visibility
const autoCenter = {}; // ID → autoCenter boolean
const breadcrumbs = {}; // ID → array of last positions
const breadcrumbLines = {}; // ID → polyline objects

// Function to update breadcrumbs for a marker
function updateBreadcrumbs(id, latLng) {
  if (!breadcrumbs[id]) {
    breadcrumbs[id] = [];
  }
  breadcrumbs[id].push(latLng);
  if (breadcrumbs[id].length > 5) {
    breadcrumbs[id].shift();
  }
  const breadcrumbCheckbox = document.getElementById(`breadcrumb${id}`);
  if (breadcrumbCheckbox && breadcrumbCheckbox.checked) {
    showBreadcrumbs(id);
  }
}

// Function to show breadcrumbs for a marker
function showBreadcrumbs(id) {
  // Remove existing breadcrumb line
  hideBreadcrumbs(id);

  // Only draw if we have at least 2 points
  if (breadcrumbs[id] && breadcrumbs[id].length > 1) {
    breadcrumbLines[id] = L.polyline(breadcrumbs[id], {
      className: "breadcrumb",
    }).addTo(map);
  }
}

// Function to hide breadcrumbs for a marker
function hideBreadcrumbs(id) {
  if (breadcrumbLines[id]) {
    map.removeLayer(breadcrumbLines[id]);
    breadcrumbLines[id] = null;
  }
}

// Function to toggle breadcrumb visibility
function toggleBreadcrumbs(id) {
  // Determine the new state based on the dropdown checkbox (or popup if dropdown doesn't exist yet)
  let isVisible;
  const dropdownCheckbox = document.getElementById(`breadcrumb${id}`);
  const popupCheckbox = document.getElementById(`popup-breadcrumb-${id}`);

  if (dropdownCheckbox) {
    // Toggle based on current state if dropdown checkbox exists
    isVisible = !dropdownCheckbox.checked;
    dropdownCheckbox.checked = isVisible;
  } else if (popupCheckbox) {
    // Fallback to popup checkbox if dropdown doesn't exist (shouldn't happen often)
    isVisible = !popupCheckbox.checked;
  } else {
    // Default to enabling if neither exists (initial click?)
    isVisible = true;
  }

  // Update the popup checkbox state to match
  if (popupCheckbox) {
    popupCheckbox.checked = isVisible;
  }

  // Show or hide the breadcrumbs
  if (isVisible) {
    showBreadcrumbs(id);
  } else {
    hideBreadcrumbs(id);
  }
}

// Ensure the leaflet-measure-path plugin is loaded correctly
if (typeof L.Control?.MeasurePath === "function") {
  // Check for L.Control.MeasurePath (class constructor)
  // Add measure control
  map.addControl(
    new L.Control.MeasurePath({
      // Instantiate with 'new'
      position: "topleft",
      showMeasurementsClearControl: true,
      showUnitControl: true,
      tooltipTextFinish: "Click to <b>finish line</b><br>",
      tooltipTextDelete: "Press SHIFT-key and click to <b>delete point</b>",
      tooltipTextMove: "Click and drag to <b>move point</b><br>",
      tooltipTextResume: "<br>Press CTRL-key and click to <b>resume line</b>",
      tooltipTextAdd: "Press CTRL-key and click to <b>add point</b>",
    })
  );
} else {
  console.warn(
    "[Leaflet2.js - map initialization] L.Control.MeasurePath is not available. Ensure the leaflet-measure-path plugin is correctly loaded."
  );
}

// Define home icon
const HOME_ICON = L.icon({
  iconUrl: "/icons/Home.png",
  iconSize: [36, 36],
  iconAnchor: [12, 41],
  popupAnchor: [1, -34],
});

// Add static home marker to the bottom layer
L.marker(HOME_LOCATION, {
  icon: HOME_ICON,
  interactive: false, // Makes marker unclickable
  keyboard: false,
})
  .addTo(map)
  .setZIndexOffset(-1000); // Set to bottom layer

function createCatDropdown(id, iconUrl, status) {
  if (dropdowns.has(id)) return; // Skip if dropdown already exists

  const baseId = KNOWN_CATS.includes(id) ? id : "generic";

  // Handle null or undefined status
  status = String(status || "").trim();

  // Use the same logic for case tolerance
  const iconStatus = findBestIconMatch(baseId, status);

  // Ensure the correct path for the icon
  const correctedIconUrl = `/icons/${baseId}_Marker_${iconStatus}.png`;

  const dropdownHtml = `
    <div class="btn-group marker-dropdown" id="dropdown-${id}">
      <button class="btn btn-primary dropdown-toggle" type="button" data-bs-toggle="dropdown">
        <img id="dropdown-icon-${id}" src="${correctedIconUrl}" style="height: 27px; margin-right: 8px; vertical-align: text-bottom;">
        ${id}
      </button>
      <ul class="dropdown-menu">
        <li><a class="dropdown-item" href="#" onclick="toggleAutoCenter('${id}')">
          <input type="checkbox" id="center${id}"> Centre and follow 🎯
        </a></li>
        <li><a class="dropdown-item" href="#" onclick="toggleVisibility('${id}')">
          <input type="checkbox" id="visible${id}" checked> Show/Hide Marker
        </a></li>
        <li><a class="dropdown-item" href="#" onclick="toggleBreadcrumbs('${id}')">
          <input type="checkbox" id="breadcrumb${id}"> Show Trail
        </a></li>
        <li>
          <a class="dropdown-item" data-bs-toggle="collapse" href="#json-data-${id}" role="button" aria-expanded="false" aria-controls="json-data-${id}">
            Show Last JSON Message
          </a>
          <div class="collapse" id="json-data-${id}">
            <pre id="json-message-${id}" class="bg-light p-2 border rounded" style="max-height: 200px; overflow-y: auto;"></pre>
          </div>
        </li>
      </ul>
    </div>`;

  const wrapper = document.createElement("div");
  wrapper.innerHTML = dropdownHtml;
  document
    .getElementById("catDropdownContainer")
    .appendChild(wrapper.firstElementChild);
  dropdowns.add(id);
  markerVisibility[id] = true; // Ensure marker is visible by default
}

// Initialize WebSocket
let ws;
let debugMessageCount = 0; // For debug console numbering
function connectWebSocket() {
  ws = new WebSocket(`ws://${window.location.hostname}:81/`);
  ws.onopen = () => console.log("WebSocket connected");
  ws.onmessage = (event) => {
    console.debug("[WS] Message received:", event.data); // Log raw incoming data
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (e) {
      console.error("[WS] Failed to parse JSON:", e, "Raw data:", event.data);
      return; // Stop processing if JSON is invalid
    }

    // --- Live JSON debug monitor logic ---
    debugMessageCount++;
    const timestamp = new Date().toISOString();
    const formattedData = {
      messageNum: debugMessageCount,
      timestamp: timestamp,
      data: data,
    };
    const jsonConsole = document.getElementById("jsonConsole");
    if (jsonConsole) {
      // Prepend new message to the top
      jsonConsole.textContent =
        JSON.stringify(formattedData, null, 2) + "\n" + jsonConsole.textContent;
    }

    if (data.id) {
      const id = data.id;
      console.debug(`[${id}] Processing data:`, JSON.stringify(data));

      // Update JSON message in dropdown (live update)
      const jsonMessage = document.getElementById(`json-message-${id}`);
      if (jsonMessage) {
        jsonMessage.textContent = JSON.stringify(data, null, 2);
      }

      // Create dropdown if it doesn't exist
      createCatDropdown(
        id,
        `/icons/${KNOWN_CATS.includes(id) ? id : "generic"}_Marker_${
          data.status || "Normal"
        }.png`,
        data.status
      );

      // Update or create marker
      let isNewMarker = false;
      if (!markers[id]) {
        console.debug(`[${id}] Creating new marker.`);
        isNewMarker = true;
        markers[id] = L.marker([0, 0], {
          // Create at [0,0] initially
          icon: getMarkerIcon(id, data.status),
        }).addTo(map); // Add to map immediately
        markerVisibility[id] = true; // Set visibility to true for new markers
        autoCenter[id] = false; // Default autoCenter to false
        breadcrumbs[id] = []; // Initialize breadcrumbs
        // Ensure the visibility checkbox reflects the default state
        const visibleCheckbox = document.getElementById(`visible${id}`);
        if (visibleCheckbox) {
          visibleCheckbox.checked = true;
        }
      }

      // Update marker position and icon
      if (data.lat && data.lon) {
        const newPos = [data.lat, data.lon];
        const currentPos = markers[id].getLatLng();
        console.debug(
          `[${id}] Current Pos: ${currentPos.lat}, ${currentPos.lng} | New Pos: ${newPos[0]}, ${newPos[1]}`
        );

        // Only update position if it has changed significantly (or if it's a new marker)
        if (
          isNewMarker ||
          currentPos.lat !== newPos[0] ||
          currentPos.lng !== newPos[1]
        ) {
          console.debug(`[${id}] Updating position to:`, newPos);
          markers[id].setLatLng(newPos);
          updateBreadcrumbs(id, newPos);
        } else {
          console.debug(`[${id}] Position unchanged.`);
        }

        // Update Icon regardless of position change, as status might have changed
        const newIcon = getMarkerIcon(id, data.status);
        // Check if icon URL actually changed before setting
        if (
          markers[id].options.icon.options.iconUrl !== newIcon.options.iconUrl
        ) {
          console.debug(
            `[${id}] Updating icon based on status: ${data.status}`
          );
          markers[id].setIcon(newIcon);
        } else {
          console.debug(`[${id}] Icon unchanged.`);
        }

        // Update dropdown icon to match marker status
        const baseId = KNOWN_CATS.includes(id) ? id : "generic";
        const status = String(data.status || "").trim();
        const iconStatus = findBestIconMatch(baseId, status);
        const iconUrl = `/icons/${baseId}_Marker_${iconStatus}.png`;
        updateDropdownIcon(id, iconUrl); // This function already checks if src needs changing

        // Auto-center if enabled
        if (autoCenter[id]) {
          console.debug(`[${id}] Auto-centering.`);
          map.panTo(newPos);
        }
      } else {
        console.warn(
          `[${id}] No valid lat/lon in data, cannot update position.`
        );
      }

      // Update marker popup (contains coordinates, status, etc.)
      // This should be called even if lat/lon didn't change, as status or other data might have
      console.debug(`[${id}] Updating popup.`);
      updateMarkerPopup(markers[id], data);

      // Pulse marker and dropdown icon on update
      pulseMarkerAndDropdown(id);
    } else {
      console.warn("[WS] Received data object without an ID:", data);
    }
  };
  ws.onerror = (error) => console.error("WebSocket error:", error);
  ws.onclose = () => {
    console.log("WebSocket closed. Attempting to reconnect...");
    // Implement a reconnect strategy, e.g., exponential backoff
    setTimeout(connectWebSocket, 5000); // Simple reconnect after 5 seconds
  };
}
connectWebSocket();

function getMarkerIcon(id, status) {
  const baseId = KNOWN_CATS.includes(id) ? id : "generic";

  // Handle empty or null status
  if (!status) status = "offline";

  // Convert status to string and normalize
  status = String(status).trim();

  // Use the findBestIconMatch function to get the proper case
  let iconStatus = findBestIconMatch(baseId, status);

  const iconUrl = `/icons/${baseId}_Marker_${iconStatus}.png`;
  console.debug(
    `[${id}] Attempting to use icon: ${iconUrl} (derived from original status: "${status}", resolved to: "${iconStatus}")`
  );

  // Create a temporary image object to test if the file exists and warn if not
  // This check does not change the iconUrl used by L.icon below.
  const img = new Image();
  img.src = iconUrl;
  img.onerror = function () {
    console.warn(
      `[${id}] Failed to pre-load icon: ${iconUrl}. Check if the file exists in /data/icons/. The marker will attempt to use this URL anyway.`
    );
    // Note: Setting this.src here doesn't change the iconUrl for the Leaflet marker.
    // If you need a true visual fallback, the file must exist, or more complex logic is needed.
  };

  return L.icon({
    iconUrl: iconUrl, // Leaflet will use this URL
    iconSize: [25, 41],
    iconAnchor: [12, 41],
    popupAnchor: [1, -34],
  });
}

// Function to check if icon file exists
// This is a client-side function that attempts to find the right case for icon files
function findBestIconMatch(baseId, status) {
  // List of possible case variations for status
  const statusVariations = [
    status, // Original case
    status.toLowerCase(), // all lowercase
    status.toUpperCase(), // ALL UPPERCASE
    status.charAt(0).toUpperCase() + status.slice(1).toLowerCase(), // Title Case
  ];

  // Standard status types to try - these are the known valid status types
  const standardTypes = ["Normal", "Error", "Offline", "Outanabout"];

  // Return the standard type if it matches one of our variations case-insensitively
  for (const stdType of standardTypes) {
    if (statusVariations.some((v) => v === stdType.toLowerCase())) {
      return stdType; // Return the correctly capitalized standard status
    }
  }

  // If no match with standard types, return the Title Case version as best guess
  return status.charAt(0).toUpperCase() + status.slice(1).toLowerCase();
}

// Consolidated Update marker popup and dropdown icon/JSON
function updateMarkerPopup(marker, data) {
  const id = data.id;
  const latlng = marker.getLatLng(); // Get current position from marker
  const coords = `${latlng.lat.toFixed(6)}, ${latlng.lng.toFixed(6)}`;
  const distance = calculateDistanceFromHome(latlng); // Ensure this function exists if needed, or remove

  // Check current states for checkboxes from markerVisibility and autoCenter maps
  const isVisible =
    markerVisibility[id] === undefined ? true : markerVisibility[id];
  const isCentered = autoCenter[id] === undefined ? false : autoCenter[id];
  // Check breadcrumb state - assumes checkbox ID `breadcrumb${id}` exists in dropdown
  const breadcrumbCheckbox = document.getElementById(`breadcrumb${id}`);
  const showTrail = breadcrumbCheckbox ? breadcrumbCheckbox.checked : false; // Default to false if checkbox not found yet

  // Popup content styled like the dropdown menu, with checkboxes for all features
  const popupContent = `
    <div class="dropdown-menu show p-2" style="min-width: 250px; position: static; box-shadow: 0 0.5rem 1rem rgba(0,0,0,0.15);">
      <h6 class="dropdown-header">${id}</h6>
      <div class="dropdown-item-text px-3 py-1">Status: ${
        data.status || "Unknown"
      }</div>
      <div class="dropdown-item-text px-3 py-1">Coords: <span class="coord-span">${coords}</span> <button class="copy-btn btn btn-sm btn-outline-secondary py-0 px-1" onclick="copyToClipboard('${coords}')">📋</button></div>
      <div class="dropdown-item-text px-3 py-1">Distance: ${distance}km</div>
      <div class="dropdown-item-text px-3 py-1 mb-2">Satellites: ${
        data.satsSeen !== undefined ? data.satsSeen : "N/A"
      }</div>
      <ul class="list-unstyled mb-2 px-2">
        <li><a class="dropdown-item" href="#" onclick="toggleAutoCenter('${id}'); return false;">
          <input type="checkbox" id="popup-center-${id}" ${
    isCentered ? "checked" : ""
  } onclick="event.stopPropagation(); toggleAutoCenter('${id}');"> Centre and follow 🎯
        </a></li>
        <li><a class="dropdown-item" href="#" onclick="toggleVisibility('${id}'); return false;">
          <input type="checkbox" id="popup-visible-${id}" ${
    isVisible ? "checked" : ""
  } onclick="event.stopPropagation(); toggleVisibility('${id}');"> Show/Hide Marker
        </a></li>
        <li><a class="dropdown-item" href="#" onclick="toggleBreadcrumbs('${id}'); return false;">
          <input type="checkbox" id="popup-breadcrumb-${id}" ${
    showTrail ? "checked" : ""
  } onclick="event.stopPropagation(); toggleBreadcrumbs('${id}');"> Show Trail
        </a></li>
      </ul>
      <div class="px-2">
        <a class="btn btn-sm btn-outline-info w-100" data-bs-toggle="collapse" href="#popup-json-data-${id}" role="button" aria-expanded="false" aria-controls="popup-json-data-${id}">
          Show Last JSON Message
        </a>
        <div class="collapse mt-2" id="popup-json-data-${id}">
          <pre class="bg-light p-2 border rounded" style="max-height: 150px; overflow-y: auto; font-size: 0.8em;">${JSON.stringify(
            data,
            null,
            2
          )}</pre>
        </div>
      </div>
    </div>`;

  marker.bindPopup(popupContent, {
    maxWidth: 300, // Adjust as needed
    className: "marker-popup", // Keep existing class if styles depend on it
  });

  // --- Also update dropdown elements ---

  // Get the appropriate icon status using our case-tolerant function
  const baseId = KNOWN_CATS.includes(id) ? id : "generic";
  const status = String(data.status || "").trim();
  const iconStatus = findBestIconMatch(baseId, status);
  const iconUrl = `/icons/${baseId}_Marker_${iconStatus}.png`;

  // Update dropdown icon source
  const dropdownIcon = document.getElementById(`dropdown-icon-${id}`);
  if (dropdownIcon) {
    // Check if the icon source needs updating to prevent unnecessary reloads
    if (dropdownIcon.src !== window.location.origin + iconUrl) {
      dropdownIcon.src = iconUrl;
      console.debug(`[${id}] Updated dropdown icon to: ${iconUrl}`);
    }
  } else {
    console.warn(`[${id}] Dropdown icon element not found for update.`);
  }

  // Update JSON message in dropdown collapse area
  const jsonMessageDiv = document.getElementById(`json-data-${id}`);
  const jsonMessagePre = document.getElementById(`json-message-${id}`);
  if (jsonMessageDiv && jsonMessagePre) {
    jsonMessagePre.textContent = JSON.stringify(data, null, 2);
  } else {
    // It's possible the dropdown hasn't been created yet when the first message arrives
    // console.warn(`[${id}] Dropdown JSON elements not found for update.`);
  }
}

// Function to toggle auto-centering of a marker
function toggleAutoCenter(id) {
  // Toggle the auto-center state
  autoCenter[id] = !autoCenter[id];

  // Update the dropdown checkbox state
  const dropdownCheckbox = document.getElementById(`center${id}`);
  if (dropdownCheckbox) {
    dropdownCheckbox.checked = autoCenter[id];
  }
  // Update the popup checkbox state (if popup is open)
  const popupCheckbox = document.getElementById(`popup-center-${id}`);
  if (popupCheckbox) {
    popupCheckbox.checked = autoCenter[id];
  }

  // If auto-center is enabled, pan the map to the marker
  if (autoCenter[id] && markers[id]) {
    map.panTo(markers[id].getLatLng());
  }
}

// Function to toggle marker visibility
function toggleVisibility(id) {
  // Toggle the visibility state
  markerVisibility[id] =
    markerVisibility[id] === undefined ? false : !markerVisibility[id];

  // Update the dropdown checkbox state
  const dropdownCheckbox = document.getElementById(`visible${id}`);
  if (dropdownCheckbox) {
    dropdownCheckbox.checked = markerVisibility[id];
  }
  // Update the popup checkbox state (if popup is open)
  const popupCheckbox = document.getElementById(`popup-visible-${id}`);
  if (popupCheckbox) {
    popupCheckbox.checked = markerVisibility[id];
  }

  // Show or hide the marker
  if (markers[id]) {
    if (markerVisibility[id]) {
      if (!map.hasLayer(markers[id])) {
        // Add only if not already on map
        map.addLayer(markers[id]);
      }
    } else {
      if (map.hasLayer(markers[id])) {
        // Remove only if on map
        map.removeLayer(markers[id]);
      }
    }
  }
}

// Improved marker pulse CSS animation for a smoother, aesthetic effect
function injectMarkerPulseCSS() {
  if (document.getElementById("marker-pulse-style")) return;
  const style = document.createElement("style");
  style.id = "marker-pulse-style";
  style.innerHTML = `
    .marker-pulse-img {
      animation: marker-pulse-anim 0.4s cubic-bezier(0.4,0,0.2,1) 0s 2;
      will-change: transform;
    }
    @keyframes marker-pulse-anim {
      0% { transform: scale(1); }
      30% { transform: scale(1.18); }
      60% { transform: scale(1); }
      100% { transform: scale(1); }
    }
    .dropdown-pulse-img {
      animation: dropdown-pulse-anim 0.4s cubic-bezier(0.4,0,0.2,1) 0s 2;
      will-change: transform, filter;
    }
    @keyframes dropdown-pulse-anim {
      0% { filter: brightness(1); transform: scale(1); }
      30% { filter: brightness(1.25); transform: scale(1.18); }
      60% { filter: brightness(1); transform: scale(1); }
      100% { filter: brightness(1); transform: scale(1); }
    }
    .dropdown-flash-green {
      animation: dropdown-flash-green-anim 1s linear 0s 1;
    }
    @keyframes dropdown-flash-green-anim {
      0% { background-color: #28ff28 !important; }
      80% { background-color: #28ff28 !important; }
      100% { background-color: inherit !important; }
    }
  `;
  document.head.appendChild(style);
}

injectMarkerPulseCSS();

function pulseMarkerAndDropdown(id) {
  // Pulse marker icon <img> inside marker
  if (markers[id] && markers[id]._icon) {
    const img = markers[id]._icon.querySelector("img");
    if (img) {
      img.classList.remove("marker-pulse-img");
      void img.offsetWidth;
      img.classList.add("marker-pulse-img");
    }
  }
  // Pulse dropdown icon <img>
  const dropdownIcon = document.getElementById(`dropdown-icon-${id}`);
  if (dropdownIcon) {
    dropdownIcon.classList.remove("dropdown-pulse-img");
    void dropdownIcon.offsetWidth;
    dropdownIcon.classList.add("dropdown-pulse-img");
  }
}

function updateDropdownIcon(id, iconUrl) {
  const dropdownIcon = document.getElementById(`dropdown-icon-${id}`);
  if (dropdownIcon) {
    dropdownIcon.src = iconUrl;
  }
}

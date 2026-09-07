const POLL_MS = 1000;
const MAX_POINTS = 60;
const NZONES = 6;
const LOW_BATTERY = 3900;   

const ZONE_VOLTAGE = { 1: "230 V", 2: "230 V", 3: "24 V", 4: "24 V", 5: "24 V", 6: "24 V" };

const el = {
  statusDot: document.getElementById("status-dot"),
  statusText: document.getElementById("status-text"),
  thr: document.getElementById("val-thr"),
  time: document.getElementById("val-time"),
  active: document.getElementById("val-active"),
  zones: document.getElementById("zones"),
  envAirTemp: document.getElementById("env-airtemp"),
  envAirHum: document.getElementById("env-airhum"),
  envLight: document.getElementById("env-light"),
  envPressure: document.getElementById("env-pressure"),
  envCo2: document.getElementById("env-co2"),
  envVoltage: document.getElementById("env-voltage"),
  battSub: document.getElementById("batt-sub"),
  sysUptime: document.getElementById("sys-uptime"),
  nodeSelect: document.getElementById("node-select"),
  lastUpdate: document.getElementById("last-update"),
  simBadge: document.getElementById("sim-badge"),
};

let selectedNode = "1";

let lastState = null;

const pendingZone = {};   

function applyPendingModes(s) {
  if (!s.zones) return;
  const now = Date.now();
  for (const id in pendingZone) {
    const p = pendingZone[id];
    if (p.until < now) {               
      delete pendingZone[id];
      continue;
    }
    
    if (s.zones[id]) {
      s.zones[id].mode = p.mode;
      if (p.relay !== null) s.zones[id].relay = p.relay;
    }
  }
}

const ctx = document.getElementById("chart").getContext("2d");
const soilGrad = ctx.createLinearGradient(0, 0, 0, 280);
soilGrad.addColorStop(0, "rgba(56,189,248,.38)");
soilGrad.addColorStop(1, "rgba(56,189,248,0)");

const chart = new Chart(ctx, {
  type: "line",
  data: {
    labels: [],
    datasets: [
      { label: "Umiditate sol (kPa)", data: [], borderColor: "#38bdf8",
        backgroundColor: soilGrad, tension: 0.35, fill: true, yAxisID: "y" },
      { label: "Temperatură aer (°C)", data: [], borderColor: "#f97316",
        backgroundColor: "rgba(249,115,22,.10)", tension: 0.35, fill: false, yAxisID: "y1" },
      { label: "Umiditate aer (%)", data: [], borderColor: "#22c55e",
        backgroundColor: "rgba(34,197,94,.10)", tension: 0.35, fill: false, yAxisID: "y1" },
    ],
  },
  options: {
    responsive: true,
    interaction: { mode: "index", intersect: false },
    elements: { point: { radius: 0, hoverRadius: 5, hitRadius: 12 }, line: { borderWidth: 2 } },
    plugins: { legend: { labels: { color: "#e2e8f0" } } },
    scales: {
      x: { ticks: { color: "#94a3b8", maxTicksLimit: 8 }, grid: { color: "#1e293b" } },
      y: { position: "left", title: { display: true, text: "kPa", color: "#38bdf8" },
           ticks: { color: "#94a3b8" }, grid: { color: "#1e293b" } },
      y1: { position: "right", title: { display: true, text: "°C / %", color: "#94a3b8" },
            ticks: { color: "#94a3b8" }, grid: { drawOnChartArea: false } },
    },
  },
});

async function fetchState() {
  try {
    const res = await fetch("/api/state");
    const s = await res.json();
    renderState(s);
    setConnected(s.connected);
    el.simBadge.hidden = !s.simulated;
  } catch (e) {
    setConnected(false);
  }
}

async function fetchHistory() {
  if (!selectedNode) return;
  try {
    const res = await fetch("/api/history?node=" + encodeURIComponent(selectedNode));
    const h = await res.json();
    const recent = (h || []).slice(-MAX_POINTS);
    chart.data.labels = recent.map((p) => formatTime(p.time));
    chart.data.datasets[0].data = recent.map((p) => p.soil);
    chart.data.datasets[1].data = recent.map((p) => p.air_temp);
    chart.data.datasets[2].data = recent.map((p) => p.air_hum);
    chart.update("none");
  } catch (e) { /* ignora */ }
}

// Randare 
function renderState(s) {
  lastState = s;
  applyPendingModes(s);
  el.thr.textContent = s.threshold ?? "--";
  el.time.textContent = s.irrigation_time ?? "--";

  let activeCount = 0;
  for (let z = 1; z <= NZONES; z++) {
    if (s.zones?.[String(z)]?.relay) activeCount++;
  }
  el.active.textContent = activeCount;

  const firstNode = s.nodes && Object.keys(s.nodes).sort((a, b) => a - b)[0];
  const env = firstNode ? s.nodes[firstNode] : null;
  if (env) {
    setMetric(el.envAirTemp, env.air_temp, "°C");
    setMetric(el.envAirHum, env.air_hum, "%");
    setMetric(el.envLight, env.light, "lx");
    setMetric(el.envPressure, env.pressure, "hPa");
    setMetric(el.envCo2, env.co2, "ppm");
    setMetric(el.envVoltage, env.voltage, "brut");

    const lowBatt = typeof env.voltage === "number" && env.voltage < LOW_BATTERY;
    el.envVoltage.classList.toggle("warn", lowBatt);
    el.battSub.classList.toggle("warn", lowBatt);
    el.battSub.textContent = lowBatt
      ? "⚠ Baterie scăzută — verifică alimentarea"
      : "nod senzor (baterie / panou solar)";
  }

  renderZones(s);
  updateNodeSelect(s);

  el.lastUpdate.textContent =
    "Ultima actualizare: " + (s.last_update ? formatTime(s.last_update) : "--");

  if (el.sysUptime) el.sysUptime.textContent = formatUptime(s.uptime);
}

function renderZones(s) {
  let html = "";
  for (let z = 1; z <= NZONES; z++) {
    const id = String(z);
    const relay = s.zones?.[id]?.relay ? 1 : 0;
    const mode = s.zones?.[id]?.mode || "auto";   
    const node = s.nodes?.[id];
    const soil = node ? node.soil : null;
    const dry = soil != null && s.threshold != null && soil >= s.threshold;

    const manual = mode === "manon" || mode === "manoff";
    const modeBadge = manual
      ? `<span class="mode manual">MANUAL</span>`
      : `<span class="mode auto">AUTO</span>`;

    html += `
      <div class="zone ${relay ? "zone-on" : ""}">
        <div class="zone-head">
          <span class="zone-title">Zona ${z} <span class="volt">${ZONE_VOLTAGE[z] || ""}</span></span>
          <span class="relay ${relay ? "on" : ""}">${relay ? "ON" : "off"}</span>
        </div>
        <div class="zone-soil">${fmt(soil)}<small>kPa</small></div>
        <div class="zone-soiltemp">Temperatură sol: ${fmt(node ? node.soil_temp : null)}<small>°C</small></div>
        <div class="zone-state ${dry ? "dry" : "wet"}">${soil == null ? "&mdash;" : (dry ? "Sol uscat" : "Sol umed")} ${modeBadge}</div>
        <div class="zone-btns">
          <button class="zbtn ${mode === "manon" ? "act-on" : ""}"  data-zone="${z}" data-cmd="on">Pornit</button>
          <button class="zbtn ${mode === "manoff" ? "act-off" : ""}" data-zone="${z}" data-cmd="off">Oprit</button>
          <button class="zbtn ${mode === "auto" ? "act-auto" : ""}" data-zone="${z}" data-cmd="auto">Auto</button>
        </div>
      </div>`;
  }
  el.zones.innerHTML = html;
}

async function sendZoneCommand(zone, value) {
  const id = String(zone);
  let mode, relay;
  if (value === "on")       { mode = "manon";  relay = 1; }
  else if (value === "off") { mode = "manoff"; relay = 0; }
  else                      { mode = "auto";   relay = null; }  
  pendingZone[id] = { mode, relay, until: Date.now() + 3000 };
  if (lastState) { applyPendingModes(lastState); renderZones(lastState); }

  try {
    await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action: "zone", zone: Number(zone), value }),
    });
    fetchState();   
  } catch (e) {
    console.error("Eroare la trimiterea comenzii:", e);
  }
}

function updateNodeSelect(s) {
  const ids = s.nodes ? Object.keys(s.nodes).sort((a, b) => a - b) : [];
  const optionsHtml = ids.map((id) => `<option value="${id}">Zona ${id}</option>`).join("");
  if (el.nodeSelect.innerHTML !== optionsHtml) {
    el.nodeSelect.innerHTML = optionsHtml;
    if (!ids.includes(selectedNode) && ids.length) {
      selectedNode = ids[0];
    }
    el.nodeSelect.value = selectedNode;
  }
}

function fmt(v) {
  return (typeof v === "number" && !isNaN(v)) ? v.toFixed(1) : "--";
}
function setMetric(node, value, unit) {
  node.innerHTML = `${fmt(value)}<small>${unit}</small>`;
}
function setConnected(connected) {
  el.statusDot.className = "dot " + (connected ? "dot-on" : "dot-off");
  el.statusText.textContent = connected ? "Conectat" : "Deconectat";
}
function formatUptime(sec) {
  if (typeof sec !== "number" || sec < 0) return "--";
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0");
}
function formatTime(iso) {
  if (!iso) return "--";
  const d = new Date(iso);
  if (isNaN(d)) return iso;
  return d.toLocaleTimeString("ro-RO", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

el.nodeSelect.addEventListener("change", () => {
  selectedNode = el.nodeSelect.value;
  fetchHistory();
});

el.zones.addEventListener("click", (ev) => {
  const btn = ev.target.closest(".zbtn");
  if (!btn) return;
  sendZoneCommand(btn.dataset.zone, btn.dataset.cmd);
});

fetchState();
fetchHistory();
setInterval(fetchState, POLL_MS);
setInterval(fetchHistory, POLL_MS);
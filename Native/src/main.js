/**
 * W.A.S.P. System Monitor — client-side dashboard logic.
 *
 * Connects to a WebSocket metrics stream, updates CPU/memory/disk/process UI,
 * manages view switching (Main / Memory / Disk / All Processes), theme toggle,
 * and sortable process tables.
 */
const WS_URL = "ws://localhost:8080/ws/metrics";

/**
 * History CSV endpoint. When the page is served over http(s), use same-origin `/api/...`
 * (e.g. python serve_dashboard.py) so fetch avoids CORS. file:// cannot talk to the API
 * without backend CORS or this proxy.
 * @returns {string}
 */
function getHistoryReportsUrl() {
  const p = window.location.protocol;
  if (p === "http:" || p === "https:") {
    return `${window.location.origin}/api/history-reports`;
  }
  return "http://127.0.0.1:8080/api/history-reports";
}

const connectionStatusEl = document.getElementById("connectionStatus");
const statusIndicatorEl = document.querySelector(".status-indicator");
const currentDateEl = document.getElementById("currentDate");
const themeToggleBtn = document.getElementById("themeToggle");
const totalCpuValueEl = document.getElementById("totalCpuValue");
const responsivenessValueEl = document.getElementById("responsivenessValue");
const memoryTextEl = document.getElementById("memoryText");
const memoryBarFillEl = document.getElementById("memoryBarFill");
const coresRowEl = document.getElementById("coresRow");
const processTableBodyEl = document.querySelector("#processTable tbody");
const viewMainEl = document.getElementById("viewMain");
const viewProcessesEl = document.getElementById("viewProcesses");
const viewMemoryEl = document.getElementById("viewMemory");
const viewDiskEl = document.getElementById("viewDisk");
const viewExportEl = document.getElementById("viewExport");
const exportStatusEl = document.getElementById("exportStatus");
const allProcessesTableBodyEl = document.querySelector("#allProcessesTable tbody");
const allProcessesSearchEl = document.getElementById("allProcessesSearch");
const memoryViewPercentEl = document.getElementById("memoryViewPercent");
const memoryViewBarFillEl = document.getElementById("memoryViewBarFill");
const memoryViewSummaryEl = document.getElementById("memoryViewSummary");
const memoryDetailsTableBodyEl = document.querySelector("#memoryDetailsTable tbody");
const diskCardsEl = document.getElementById("diskCards");

let socket = null;
let lastProcessesData = null;

const PRIORITY_ORDER = ["idle", "below normal", "normal", "above normal", "high", "realtime"];
let processSortBy = "name";
let processSortDir = "asc";

/**
 * Maps a Windows-style process priority string to a numeric rank for sorting.
 * Unknown values sort after known priorities.
 * @param {string} p - Raw priority (e.g. "NORMAL", "ABOVE_NORMAL").
 * @returns {number} Index in PRIORITY_ORDER, or PRIORITY_ORDER.length if unknown.
 */
function priorityRank(p) {
  let s = String(p || "").toLowerCase().replace(/_/g, " ").trim();
  if (s === "real time") s = "realtime";
  const i = PRIORITY_ORDER.indexOf(s);
  return i >= 0 ? i : PRIORITY_ORDER.length;
}

/**
 * Comparator for two process objects using global processSortBy / processSortDir.
 * @param {object} a
 * @param {object} b
 * @returns {number} Negative if a before b, positive if after, 0 if equal.
 */
function compareProcesses(a, b) {
  let va, vb;
  switch (processSortBy) {
    case "name":
      va = (a.name || "").toLowerCase();
      vb = (b.name || "").toLowerCase();
      return processSortDir === "asc" ? va.localeCompare(vb) : vb.localeCompare(va);
    case "owner":
      va = (a.owner || "").toLowerCase();
      vb = (b.owner || "").toLowerCase();
      return processSortDir === "asc" ? va.localeCompare(vb) : vb.localeCompare(va);
    case "pid":
      va = Number(a.pid) || 0;
      vb = Number(b.pid) || 0;
      return processSortDir === "asc" ? va - vb : vb - va;
    case "priority":
      va = priorityRank(a.priority);
      vb = priorityRank(b.priority);
      return processSortDir === "asc" ? va - vb : vb - va;
    case "cpu_percent":
      va = Number(a.cpu_percent) || 0;
      vb = Number(b.cpu_percent) || 0;
      return processSortDir === "asc" ? va - vb : vb - va;
    case "cpu_time_100ns":
      va = Number(a.cpu_time_100ns) || 0;
      vb = Number(b.cpu_time_100ns) || 0;
      return processSortDir === "asc" ? va - vb : vb - va;
    case "location":
      va = (a.location || "").toLowerCase();
      vb = (b.location || "").toLowerCase();
      return processSortDir === "asc" ? va.localeCompare(vb) : vb.localeCompare(va);
    default:
      return 0;
  }
}

/**
 * Returns a new array of processes sorted by the current column and direction.
 * @param {object[]} list
 * @returns {object[]}
 */
function sortProcesses(list) {
  return list.slice().sort(compareProcesses);
}

/**
 * Updates chevron labels on all sortable process table headers to match current sort.
 */
function updateSortUI() {
  document.querySelectorAll(".processTable thead th[data-sort]").forEach((th) => {
    const key = th.getAttribute("data-sort");
    const chevron = th.querySelector(".sort-chevron");
    if (!chevron) return;
    if (key === processSortBy) {
      chevron.textContent = processSortDir === "asc" ? " ▲" : " ▼";
      chevron.classList.add("active");
    } else {
      chevron.textContent = " ▲▼";
      chevron.classList.remove("active");
    }
  });
}
let cpuHistory = [];
let memoryHistory = [];
let responsivenessHistory = [];
const MAX_POINTS = 30; // ~25-30 seconds at 1s updates

/**
 * Sets sidebar connection status text and CSS class (e.g. status-ok / status-bad).
 * @param {string} text
 * @param {string} cssClass
 */
function setConnectionStatus(text, cssClass) {
  connectionStatusEl.textContent = text;
  connectionStatusEl.className = cssClass;
  if (statusIndicatorEl) {
    const connected = cssClass === "status-ok";
    statusIndicatorEl.classList.toggle("connected", connected);
    statusIndicatorEl.classList.toggle("disconnected", !connected);
  }
}

/**
 * Writes the current local date/time into the sidebar footer element.
 */
function setCurrentDate() {
  if (!currentDateEl) return;
  const now = new Date();
  const options = {
    weekday: "short",
    year: "numeric",
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  };
  currentDateEl.textContent = now.toLocaleString(undefined, options);
}

const THEME_STORAGE_KEY = "wasp-system-monitor-theme";

/**
 * Applies light or dark theme by toggling body.light-theme.
 * Default development theme is dark.
 * @param {"light"|"dark"} theme
 */
function applyTheme(theme) {
  const root = document.body;
  if (!root) return;
  if (theme === "light") {
    root.classList.add("light-theme");
  } else {
    root.classList.remove("light-theme");
  }
}

/**
 * Restores theme from localStorage, or falls back to prefers-color-scheme.
 */
function initTheme() {
  let stored = null;
  try {
    stored = window.localStorage.getItem(THEME_STORAGE_KEY);
  } catch (_) {}

  if (stored === "light" || stored === "dark") {
    applyTheme(stored);
    return;
  }

  const prefersLight =
    window.matchMedia &&
    window.matchMedia("(prefers-color-scheme: light)").matches;
  applyTheme(prefersLight ? "light" : "dark");
}

/**
 * Cycles light/dark theme and persists the choice to localStorage.
 */
function toggleTheme() {
  const isLight = document.body.classList.toggle("light-theme");
  try {
    window.localStorage.setItem(THEME_STORAGE_KEY, isLight ? "light" : "dark");
  } catch (_) {}
}

/**
 * Opens the metrics WebSocket, parses JSON payloads, and drives updateDashboard.
 * Reconnects after close/error with a short delay.
 */
function connectWebSocket() {
  socket = new WebSocket(WS_URL);

  socket.onopen = () => {
    setConnectionStatus("Connected", "status-ok");
  };

  socket.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      updateDashboard(data);
    } catch (err) {
      console.error("Failed to parse JSON message", err);
    }
  };

  socket.onclose = () => {
    setConnectionStatus("Disconnected – retrying…", "status-bad");
    setTimeout(connectWebSocket, 5000);
  };

  socket.onerror = () => {
    setConnectionStatus("Error – retrying…", "status-bad");
    socket.close();
  };
}

/**
 * Single entry point after each metrics message: refreshes all dashboard sections.
 * @param {object} data - Parsed JSON from the WebSocket (cpu, memory, disk, processes, etc.).
 */
function updateDashboard(data) {
  updateSummaryPanels(data);
  updateMemoryView(data);
  updateDiskView(data);
  updateCores(data);
  updateProcesses(data);
  if (Array.isArray(data.processes)) lastProcessesData = data;
  updateAllProcesses(lastProcessesData);
  updateCpuHistory(data);
}

/**
 * Formats a byte count as GiB with one decimal place.
 * @param {number} bytes
 * @returns {string}
 */
function formatBytesToGiB(bytes) {
  if (typeof bytes !== "number") return "–";
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GiB`;
}

/**
 * Formats a memory object field for display in the Memory details table.
 * Byte fields get raw + GiB; numbers get locale formatting; others stringify.
 * @param {string} key
 * @param {*} value
 * @returns {string}
 */
function formatMemoryField(key, value) {
  if (typeof value === "number" && key.endsWith("_bytes")) {
    return `${value.toLocaleString()} (${formatBytesToGiB(value)})`;
  }
  if (typeof value === "number") {
    return value.toLocaleString();
  }
  return String(value ?? "–");
}

/**
 * Converts bytes per second to a human-readable rate (B/s, KB/s, or MB/s).
 * @param {number} bytesPerSec
 * @returns {string}
 */
function formatRate(bytesPerSec) {
  if (typeof bytesPerSec !== "number") return "–";
  const kb = bytesPerSec / 1024;
  const mb = kb / 1024;
  if (mb >= 1) return `${mb.toFixed(2)} MB/s`;
  if (kb >= 1) return `${kb.toFixed(2)} KB/s`;
  return `${bytesPerSec.toFixed(0)} B/s`;
}

/**
 * Turns a snake_case JSON key into Title Case labels for detail tables.
 * @param {string} key
 * @returns {string}
 */
function labelFromKey(key) {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (c) => c.toUpperCase());
}

/**
 * Updates the dedicated Memory tab: gauge, summary, and all fields from data.memory.
 * @param {object} data
 */
function updateMemoryView(data) {
  const memory = data?.memory;
  if (!memory) return;

  // Pull the three values we use for the hero gauge; keep null if missing or wrong type
  // so we can show "–" instead of NaN or bogus layout.
  const usedPercent =
    typeof memory.memory_usage_percent === "number" ? memory.memory_usage_percent : null;
  const usedBytes = typeof memory.used_bytes === "number" ? memory.used_bytes : null;
  const totalBytes = typeof memory.total_bytes === "number" ? memory.total_bytes : null;

  if (memoryViewPercentEl) {
    memoryViewPercentEl.textContent = usedPercent !== null ? `${usedPercent.toFixed(1)}%` : "–";
  }
  if (memoryViewBarFillEl && usedPercent !== null) {
    const clamped = Math.max(0, Math.min(100, usedPercent));
    memoryViewBarFillEl.style.width = `${clamped}%`;
  }

  // Subtext: used vs total capacity in GiB (only if both byte counts exist).
  if (memoryViewSummaryEl) {
    if (usedBytes !== null && totalBytes !== null) {
      memoryViewSummaryEl.textContent = `${formatBytesToGiB(usedBytes)} / ${formatBytesToGiB(totalBytes)}`;
    } else {
      memoryViewSummaryEl.textContent = "–";
    }
  }

  // "Memory Details": one row per key on the memory object (total_bytes, page_fault_count, etc.).
  if (!memoryDetailsTableBodyEl) return;
  memoryDetailsTableBodyEl.innerHTML = "";
  Object.entries(memory).forEach(([key, value]) => {
    const row = document.createElement("tr");
    row.innerHTML = `
      <td>${labelFromKey(key)}</td>
      <td>${formatMemoryField(key, value)}</td>
    `;
    memoryDetailsTableBodyEl.appendChild(row);
  });
}

/**
 * Formats a disk object field for display (bytes, speeds, numbers, strings).
 * @param {string} key
 * @param {*} value
 * @returns {string}
 */
function formatDiskField(key, value) {
  if (typeof value === "number" && key.endsWith("_bytes")) {
    if (key.includes("speed")) {
      return `${value.toLocaleString()} (${formatRate(value)})`;
    }
    return `${value.toLocaleString()} (${formatBytesToGiB(value)})`;
  }
  if (typeof value === "number") {
    return value.toLocaleString();
  }
  return String(value ?? "–");
}

/**
 * Renders the Disk tab: one card per drive with usage gauge and full field list.
 * @param {object} data
 */
function updateDiskView(data) {
  if (!diskCardsEl) return;
  const disks = Array.isArray(data?.disk) ? data.disk : [];
  diskCardsEl.innerHTML = "";

  if (disks.length === 0) {
    const empty = document.createElement("div");
    empty.className = "disk-card";
    empty.textContent = "No disk data available.";
    diskCardsEl.appendChild(empty);
    return;
  }

  disks.forEach((disk) => {
    const totalBytes = typeof disk.total_bytes === "number" ? disk.total_bytes : 0;
    const freeBytes = typeof disk.free_bytes === "number" ? disk.free_bytes : 0;
    const usedBytes = Math.max(0, totalBytes - freeBytes);
    const usedPct = totalBytes > 0 ? (usedBytes / totalBytes) * 100 : 0;

    const card = document.createElement("article");
    card.className = "disk-card";

    const detailsRows = Object.entries(disk)
      .map(
        ([key, value]) => `
          <tr>
            <td>${labelFromKey(key)}</td>
            <td>${formatDiskField(key, value)}</td>
          </tr>
        `
      )
      .join("");

    card.innerHTML = `
      <div class="disk-card-header">
        <div class="disk-drive">${disk.drive_letter || "Drive"}</div>
        <div class="disk-percent">${usedPct.toFixed(1)}% used</div>
      </div>
      <div class="memory-bar">
        <div class="memory-bar-fill disk-bar-fill" style="width:${Math.max(0, Math.min(100, usedPct))}%;"></div>
      </div>
      <div class="disk-subtext">${formatBytesToGiB(usedBytes)} / ${formatBytesToGiB(totalBytes)}</div>
      <table class="disk-details">
        <tbody>
          ${detailsRows}
        </tbody>
      </table>
    `;
    diskCardsEl.appendChild(card);
  });
}

/**
 * Updates main dashboard summary tiles: CPU %, responsiveness, and memory strip.
 * @param {object} data
 */
function updateSummaryPanels(data) {
  if (data.cpu) {
    if (typeof data.cpu.cpu_usage_percent === "number") {
      totalCpuValueEl.textContent = `${data.cpu.cpu_usage_percent.toFixed(1)}%`;
    }
    if (typeof data.cpu.system_responsiveness_percent === "number") {
      responsivenessValueEl.textContent = `${data.cpu.system_responsiveness_percent.toFixed(
        1
      )}%`;
    }
  }

  if (data.memory && typeof data.memory.memory_usage_percent === "number") {
    const usedPercent = data.memory.memory_usage_percent;
    const totalBytes = data.memory.total_bytes;
    const usedBytes = data.memory.used_bytes;

    if (memoryTextEl) {
      if (typeof totalBytes === "number" && typeof usedBytes === "number") {
        const bytesToGiB = (bytes) => bytes / (1024 * 1024 * 1024);
        const usedGiB = bytesToGiB(usedBytes);
        const totalGiB = bytesToGiB(totalBytes);
        memoryTextEl.textContent = `${usedPercent.toFixed(0)}% (${usedGiB.toFixed(
          1
        )} / ${totalGiB.toFixed(1)} GiB)`;
      } else {
        memoryTextEl.textContent = `${usedPercent.toFixed(0)}%`;
      }
    }

    if (memoryBarFillEl) {
      const clamped = Math.max(0, Math.min(100, usedPercent));
      memoryBarFillEl.style.width = `${clamped}%`;
    }
  }
}

/**
 * Rebuilds per-core radial gauges from data.cpu_cores.
 * @param {object} data
 */
function updateCores(data) {
  if (!Array.isArray(data.cpu_cores)) return;

  coresRowEl.innerHTML = "";
  data.cpu_cores.forEach((core) => {
    const usage = typeof core.core_usage_percent === "number" ? core.core_usage_percent : 0;
    const coreEl = document.createElement("div");
    coreEl.className = "core";
    coreEl.innerHTML = `
      <div class="core-label">CPU ${core.core_index}</div>
      <div class="core-gauge">
        <div class="core-gauge-fill" style="--usage:${usage};"></div>
        <div class="core-gauge-text">${usage.toFixed(0)}%</div>
      </div>
    `;
    coresRowEl.appendChild(coreEl);
  });
}

/**
 * Fills the Main view “Active Processes” table (CPU% > 0 only), sorted by current sort state.
 * @param {object} data
 */
function updateProcesses(data) {
  if (!Array.isArray(data.processes)) return;

  const filtered = data.processes.filter((p) => (p.cpu_percent || 0) > 0);
  const sorted = sortProcesses(filtered);

  processTableBodyEl.innerHTML = "";
  sorted.forEach((proc) => {
    const tr = document.createElement("tr");
    if ((proc.cpu_percent || 0) > 50) {
      tr.classList.add("high-usage");
    }
    tr.innerHTML = `
      <td>${proc.name || ""}</td>
      <td>${proc.owner || ""}</td>
      <td>${proc.pid || ""}</td>
      <td>${proc.priority || ""}</td>
      <td>${(proc.cpu_percent || 0).toFixed(1)}</td>
      <td>${proc.cpu_time_100ns || ""}</td>
      <td>${proc.location || ""}</td>
    `;
    processTableBodyEl.appendChild(tr);
  });
}

/**
 * Fills the All Processes table with every process, filtered by search box, sorted.
 * @param {object|null} data
 */
function updateAllProcesses(data) {
  if (!allProcessesTableBodyEl || !Array.isArray(data?.processes)) return;

  const searchTerm = (allProcessesSearchEl?.value || "").toLowerCase();
  const filtered = data.processes.filter((p) => {
    const haystack = `${p.name || ""} ${p.owner || ""} ${p.pid || ""} ${p.location || ""}`.toLowerCase();
    return haystack.includes(searchTerm);
  });
  const sorted = sortProcesses(filtered);

  allProcessesTableBodyEl.innerHTML = "";
  sorted.forEach((proc) => {
    const tr = document.createElement("tr");
    if ((proc.cpu_percent || 0) > 50) {
      tr.classList.add("high-usage");
    }
    tr.innerHTML = `
      <td>${proc.name || ""}</td>
      <td>${proc.owner || ""}</td>
      <td>${proc.pid || ""}</td>
      <td>${proc.priority || ""}</td>
      <td>${(proc.cpu_percent || 0).toFixed(1)}</td>
      <td>${proc.cpu_time_100ns || ""}</td>
      <td>${proc.location || ""}</td>
    `;
    allProcessesTableBodyEl.appendChild(tr);
  });
}

/**
 * Appends latest CPU %, memory %, and responsiveness % to rolling histories and redraws the chart.
 * @param {object} data
 */
function updateCpuHistory(data) {
  if (data.cpu && typeof data.cpu.cpu_usage_percent === "number") {
    cpuHistory.push(data.cpu.cpu_usage_percent);
    if (cpuHistory.length > MAX_POINTS) {
      cpuHistory.shift();
    }
  }

  if (data.memory && typeof data.memory.memory_usage_percent === "number") {
    memoryHistory.push(data.memory.memory_usage_percent);
    if (memoryHistory.length > MAX_POINTS) {
      memoryHistory.shift();
    }
  }

  if (
    data.cpu &&
    typeof data.cpu.system_responsiveness_percent === "number"
  ) {
    responsivenessHistory.push(data.cpu.system_responsiveness_percent);
    if (responsivenessHistory.length > MAX_POINTS) {
      responsivenessHistory.shift();
    }
  }

  if (
    cpuHistory.length === 0 &&
    memoryHistory.length === 0 &&
    responsivenessHistory.length === 0
  )
    return;
  drawCpuHistory();
}

/**
 * Draws the live multi-series line chart on the Main view canvas (CPU, memory, responsiveness).
 * Sizes the canvas to its CSS box so it stays sharp after view switches.
 */
function drawCpuHistory() {
  const canvas = document.getElementById("cpuUsageChart");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");

  const width = (canvas.width = canvas.clientWidth || 600);
  const height = (canvas.height = canvas.clientHeight || 200);

  ctx.clearRect(0, 0, width, height);

  const maxLen = Math.max(
    cpuHistory.length,
    memoryHistory.length,
    responsivenessHistory.length
  );
  if (maxLen < 2) return;

  // CPU line (orange)
  if (cpuHistory.length > 0) {
    ctx.strokeStyle = "#f39c12";
    ctx.lineWidth = 2;
    ctx.beginPath();

    cpuHistory.forEach((value, index) => {
      const x = (index / (MAX_POINTS - 1)) * width;
      const y = height - (value / 100) * height;
      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });

    ctx.stroke();
  }

  // Memory line (blue)
  if (memoryHistory.length > 0) {
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 2;
    ctx.beginPath();

    memoryHistory.forEach((value, index) => {
      const x = (index / (MAX_POINTS - 1)) * width;
      const y = height - (value / 100) * height;
      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });

    ctx.stroke();
  }

  // Responsiveness line (dotted red)
  if (responsivenessHistory.length > 0) {
    ctx.strokeStyle = "#ff5252";
    ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 3]);
    ctx.beginPath();

    responsivenessHistory.forEach((value, index) => {
      const x = (index / (MAX_POINTS - 1)) * width;
      const y = height - (value / 100) * height;
      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });

    ctx.stroke();
    ctx.setLineDash([]);
  }

  // Legend in top-left
  const legendX = 8;
  let legendY = 8;
  const legendGap = 14;

  ctx.font = "11px system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif";
  ctx.textBaseline = "top";

  if (cpuHistory.length > 0) {
    ctx.fillStyle = "#f39c12";
    ctx.fillRect(legendX, legendY + 4, 12, 3);
    ctx.fillStyle = "#ffffff";
    ctx.fillText("CPU %", legendX + 18, legendY);
    legendY += legendGap;
  }

  if (memoryHistory.length > 0) {
    ctx.fillStyle = "#4da3ff";
    ctx.fillRect(legendX, legendY + 4, 12, 3);
    ctx.fillStyle = "#ffffff";
    ctx.fillText("Memory %", legendX + 18, legendY);
    legendY += legendGap;
  }

  if (responsivenessHistory.length > 0) {
    ctx.fillStyle = "#ff5252";
    ctx.fillRect(legendX, legendY + 4, 12, 3);
    ctx.fillStyle = "#ffffff";
    ctx.fillText("Responsiveness %", legendX + 18, legendY);
  }
}

// Theme toggle in header: persists choice and toggles body.light-theme.
if (themeToggleBtn) {
  themeToggleBtn.addEventListener("click", toggleTheme);
}

/**
 * Fetches the bundled history CSV from the backend (header + 3 rows: 5m, 30m, 1h).
 * @returns {Promise<string>}
 */
async function fetchHistoryReportsCsv() {
  const res = await fetch(getHistoryReportsUrl(), { method: "GET" });
  if (!res.ok) {
    throw new Error(`Request failed (${res.status} ${res.statusText})`);
  }
  return res.text();
}

/**
 * Splits the API CSV into header and three metric rows in order.
 * @param {string} text - Raw CSV body from `/api/history-reports`.
 * @returns {{ header: string, rows: [string, string, string] }}
 */
function splitHistoryReportCsv(text) {
  const lines = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").trim().split("\n");
  if (lines.length < 4) {
    throw new Error("Unexpected CSV: expected a header and 3 data rows.");
  }
  return {
    header: lines[0],
    rows: [lines[1], lines[2], lines[3]],
  };
}

/**
 * Triggers a file download in the browser from text content.
 * @param {string} filename
 * @param {string} csvBody - Full CSV including trailing newline if desired.
 */
function downloadTextFile(filename, csvBody) {
  const blob = new Blob(["\uFEFF", csvBody], { type: "text/csv;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

const EXPORT_WINDOW_FILES = {
  "5m": { rowIndex: 0, filename: "history-report-5m.csv" },
  "30m": { rowIndex: 1, filename: "history-report-30m.csv" },
  "1h": { rowIndex: 2, filename: "history-report-1h.csv" },
};

/**
 * Downloads a single-window CSV by fetching `/api/history-reports` and extracting one row.
 * @param {"5m"|"30m"|"1h"} windowKey
 */
async function downloadHistoryReportForWindow(windowKey) {
  const meta = EXPORT_WINDOW_FILES[windowKey];
  if (!meta) return;

  if (exportStatusEl) exportStatusEl.textContent = "";

  try {
    const raw = await fetchHistoryReportsCsv();
    const { header, rows } = splitHistoryReportCsv(raw);
    const row = rows[meta.rowIndex];
    const csvBody = `${header}\n${row}\n`;
    downloadTextFile(meta.filename, csvBody);
    if (exportStatusEl) {
      exportStatusEl.textContent = `Saved ${meta.filename}`;
    }
  } catch (err) {
    console.error(err);
    const msg = err instanceof Error ? err.message : String(err);
    if (exportStatusEl) {
      let hint = "";
      if (window.location.protocol === "file:") {
        hint =
          " Open via http://127.0.0.1:8765/index.html (run python serve_dashboard.py from the project folder) to avoid CORS.";
      }
      exportStatusEl.textContent = `Could not download report: ${msg}.${hint}`;
    }
  }
}

// Export view: each button downloads one aggregation window from the same API response shape.
document.querySelectorAll(".export-download-btn[data-export-window]").forEach((btn) => {
  btn.addEventListener("click", () => {
    const key = btn.getAttribute("data-export-window");
    if (key === "5m" || key === "30m" || key === "1h") {
      downloadHistoryReportForWindow(key);
    }
  });
});

// Sidebar: switch content views (Main / Memory / Disk / All Processes / Export Report).
document.querySelectorAll(".nav-item[data-view]").forEach((btn) => {
  btn.addEventListener("click", () => {
    const view = btn.getAttribute("data-view");
    document.querySelectorAll(".nav-item[data-view]").forEach((b) => b.classList.remove("active"));
    btn.classList.add("active");
    if (viewMainEl) viewMainEl.classList.toggle("active", view === "main");
    if (viewMemoryEl) viewMemoryEl.classList.toggle("active", view === "memory");
    if (viewDiskEl) viewDiskEl.classList.toggle("active", view === "disk");
    if (viewProcessesEl) viewProcessesEl.classList.toggle("active", view === "processes");
    if (viewExportEl) viewExportEl.classList.toggle("active", view === "export");
    if (view === "main") {
      requestAnimationFrame(() => drawCpuHistory());
    }
  });
});

// Process tables: click column header to change sort column/direction and refresh both tables.
document.querySelectorAll(".processTable").forEach((table) => {
  table.addEventListener("click", (e) => {
    const th = e.target.closest("th.sortable[data-sort]");
    if (!th) return;
    const key = th.getAttribute("data-sort");
    if (processSortBy === key) {
      processSortDir = processSortDir === "asc" ? "desc" : "asc";
    } else {
      processSortBy = key;
      processSortDir = "asc";
    }
    updateSortUI();
    if (lastProcessesData) {
      updateProcesses(lastProcessesData);
      updateAllProcesses(lastProcessesData);
    }
  });
});

// All Processes view: filter full list client-side on each keystroke.
if (allProcessesSearchEl) {
  allProcessesSearchEl.addEventListener("input", () => {
    if (lastProcessesData) updateAllProcesses(lastProcessesData);
  });
}

// Boot: align sort chevrons with default sort, apply theme, start clock + WebSocket.
updateSortUI();
initTheme();
setCurrentDate();
setInterval(setCurrentDate, 60 * 1000);
connectWebSocket();


/* gtopt GUI Service – Main Application Logic */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let schemas = {};
let currentTab = "options";
let currentElementType = null;
let currentElementView = "card";
let spreadsheetInstance = null;
let resultsData = null;
let resultsChart = null;
let chartLinePopupIndex = -1;
let _selectedColumns = new Set();
let _resultsCurrentKey = null;
let solverJobId = null;
let solverPollTimer = null;
let webserviceUrl = "";
const SCHEMA_RETRY_BUDGET_MS = 2000;
const SCHEMA_RETRY_DELAY_MS = 400;

const caseData = {
  case_name: "my_case",
  options: {},
  simulation: {
    block_array: [],
    stage_array: [],
    scenario_array: [],
  },
  system: {},
  data_files: {},
};

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

async function fetchSchemasWithRetry(budgetMs = SCHEMA_RETRY_BUDGET_MS, delayMs = SCHEMA_RETRY_DELAY_MS) {
  const deadline = Date.now() + Math.max(0, budgetMs);
  do {
    try {
      const resp = await fetch("/api/schemas");
      if (resp.ok) return await resp.json();
    } catch (e) {
      // Retry while service is still warming up.
    }
    const remainingMs = deadline - Date.now();
    if (remainingMs <= 0) break;
    await new Promise((resolve) => setTimeout(resolve, Math.min(delayMs, remainingMs)));
  } while (true);
  return {};
}

document.addEventListener("DOMContentLoaded", async () => {
  // Set up nav clicks first so static tabs remain responsive even if schema
  // loading is delayed during service startup.
  document.querySelectorAll(".nav-link").forEach((link) => {
    link.addEventListener("click", (e) => {
      e.preventDefault();
      switchTab(link.dataset.tab);
    });
  });

  schemas = await fetchSchemasWithRetry();
  updateBadges();

  // Load webservice config
  try {
    const cfgResp = await fetch("/api/solve/config");
    const cfg = await cfgResp.json();
    webserviceUrl = cfg.webservice_url || "";
    const wsInput = document.getElementById("wsUrl");
    if (wsInput) wsInput.value = webserviceUrl;
  } catch (e) {
    // Ignore – config endpoint may not be available
  }
});

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

function switchTab(tab) {
  currentTab = tab;

  // Update nav links
  document.querySelectorAll(".nav-link").forEach((l) => l.classList.remove("active"));
  const activeLink = document.querySelector(`.nav-link[data-tab="${tab}"]`);
  if (activeLink) activeLink.classList.add("active");

  // Hide all panels
  document.querySelectorAll(".tab-panel").forEach((p) => (p.style.display = "none"));
  document.getElementById("panel-element").style.display = "none";

  if (tab === "options") {
    showPanel("panel-options");
  } else if (tab === "simulation") {
    showPanel("panel-simulation");
    renderSimulation();
  } else if (tab === "results") {
    showPanel("panel-results");
  } else if (tab === "logs") {
    showPanel("panel-logs");
    refreshLogs();
  } else if (tab === "solver") {
    showPanel("panel-solver");
  } else if (schemas[tab]) {
    currentElementType = tab;
    document.getElementById("panel-element").style.display = "block";
    document.getElementById("panel-element").classList.add("active");
    document.getElementById("elementTitle").textContent = schemas[tab].label + "s";
    document.getElementById("addElementBtn").textContent = "+ Add " + schemas[tab].label;
    renderCurrentElementView();
  }
}

function showPanel(id) {
  const panel = document.getElementById(id);
  if (panel) {
    panel.style.display = "block";
    panel.classList.add("active");
  }
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

function gatherOptions() {
  const opts = {};
  const fields = [
    "annual_discount_rate",
    "demand_fail_cost",
    "reserve_fail_cost",
    "scale_objective",
    "scale_theta",
    "kirchhoff_threshold",
    "input_directory",
    "input_format",
    "output_directory",
    "output_format",
    "output_compression",
  ];
  const boolFields = ["use_line_losses", "use_kirchhoff", "use_single_bus", "use_lp_names", "use_uid_fname"];

  for (const f of fields) {
    const el = document.getElementById("opt-" + f);
    if (!el) continue;
    const v = el.value.trim();
    if (v === "") continue;
    const num = Number(v);
    opts[f] = isNaN(num) ? v : num;
  }
  for (const f of boolFields) {
    const el = document.getElementById("opt-" + f);
    if (!el || el.value === "") continue;
    opts[f] = el.value === "true";
  }
  return opts;
}

function loadOptions(opts) {
  if (!opts) return;
  for (const [k, v] of Object.entries(opts)) {
    const el = document.getElementById("opt-" + k);
    if (!el) continue;
    if (typeof v === "boolean") {
      el.value = v ? "true" : "false";
    } else {
      el.value = v;
    }
  }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

function renderSimulation() {
  renderBlocksTable();
  renderStagesTable();
  renderScenariosTable();
}

function renderBlocksTable() {
  const tbody = document.querySelector("#blocksTable tbody");
  tbody.innerHTML = "";
  const blocks = caseData.simulation.block_array || [];
  blocks.forEach((b, i) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td><input type="number" value="${b.uid}" onchange="updateBlock(${i},'uid',this.value)"></td>
      <td><input type="text" value="${b.name || ""}" onchange="updateBlock(${i},'name',this.value)"></td>
      <td><input type="number" step="any" value="${b.duration}" onchange="updateBlock(${i},'duration',this.value)"></td>
      <td><button class="btn-danger" onclick="removeBlock(${i})">Remove</button></td>`;
    tbody.appendChild(tr);
  });
}

function addBlock() {
  const blocks = caseData.simulation.block_array;
  const uid = blocks.length ? Math.max(...blocks.map((b) => b.uid)) + 1 : 1;
  blocks.push({ uid, duration: 1 });
  renderBlocksTable();
}

function updateBlock(i, field, value) {
  if (field === "name") {
    caseData.simulation.block_array[i][field] = value || undefined;
  } else if (field === "uid") {
    caseData.simulation.block_array[i][field] = parseInt(value, 10);
  } else {
    caseData.simulation.block_array[i][field] = Number(value);
  }
}

function removeBlock(i) {
  caseData.simulation.block_array.splice(i, 1);
  renderBlocksTable();
}

function renderStagesTable() {
  const tbody = document.querySelector("#stagesTable tbody");
  tbody.innerHTML = "";
  const stages = caseData.simulation.stage_array || [];
  stages.forEach((s, i) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td><input type="number" value="${s.uid}" onchange="updateStage(${i},'uid',this.value)"></td>
      <td><input type="text" value="${s.name || ""}" onchange="updateStage(${i},'name',this.value)"></td>
      <td><input type="number" value="${s.first_block}" onchange="updateStage(${i},'first_block',this.value)"></td>
      <td><input type="number" value="${s.count_block}" onchange="updateStage(${i},'count_block',this.value)"></td>
      <td><input type="number" step="any" value="${s.discount_factor != null ? s.discount_factor : ""}" onchange="updateStage(${i},'discount_factor',this.value)"></td>
      <td><select onchange="updateStage(${i},'active',this.value)">
        <option value="1" ${s.active !== 0 ? "selected" : ""}>Yes</option>
        <option value="0" ${s.active === 0 ? "selected" : ""}>No</option>
      </select></td>
      <td><button class="btn-danger" onclick="removeStage(${i})">Remove</button></td>`;
    tbody.appendChild(tr);
  });
}

function addStage() {
  const stages = caseData.simulation.stage_array;
  const uid = stages.length ? Math.max(...stages.map((s) => s.uid)) + 1 : 1;
  const blocks = caseData.simulation.block_array;
  stages.push({
    uid,
    first_block: blocks.length ? blocks.length - 1 : 0,
    count_block: 1,
    active: 1,
  });
  renderStagesTable();
}

function updateStage(i, field, value) {
  if (field === "name") {
    caseData.simulation.stage_array[i][field] = value || undefined;
  } else if (field === "discount_factor") {
    caseData.simulation.stage_array[i][field] = value ? Number(value) : undefined;
  } else {
    caseData.simulation.stage_array[i][field] = parseInt(value, 10);
  }
}

function removeStage(i) {
  caseData.simulation.stage_array.splice(i, 1);
  renderStagesTable();
}

function renderScenariosTable() {
  const tbody = document.querySelector("#scenariosTable tbody");
  tbody.innerHTML = "";
  const scenarios = caseData.simulation.scenario_array || [];
  scenarios.forEach((sc, i) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td><input type="number" value="${sc.uid}" onchange="updateScenario(${i},'uid',this.value)"></td>
      <td><input type="text" value="${sc.name || ""}" onchange="updateScenario(${i},'name',this.value)"></td>
      <td><input type="number" step="any" value="${sc.probability_factor != null ? sc.probability_factor : 1}" onchange="updateScenario(${i},'probability_factor',this.value)"></td>
      <td><select onchange="updateScenario(${i},'active',this.value)">
        <option value="1" ${sc.active !== 0 ? "selected" : ""}>Yes</option>
        <option value="0" ${sc.active === 0 ? "selected" : ""}>No</option>
      </select></td>
      <td><button class="btn-danger" onclick="removeScenario(${i})">Remove</button></td>`;
    tbody.appendChild(tr);
  });
}

function addScenario() {
  const scenarios = caseData.simulation.scenario_array;
  const uid = scenarios.length ? Math.max(...scenarios.map((s) => s.uid)) + 1 : 1;
  scenarios.push({ uid, probability_factor: 1 });
  renderScenariosTable();
}

function updateScenario(i, field, value) {
  if (field === "name") {
    caseData.simulation.scenario_array[i][field] = value || undefined;
  } else if (field === "probability_factor") {
    caseData.simulation.scenario_array[i][field] = Number(value);
  } else {
    caseData.simulation.scenario_array[i][field] = parseInt(value, 10);
  }
}

function removeScenario(i) {
  caseData.simulation.scenario_array.splice(i, 1);
  renderScenariosTable();
}

// ---------------------------------------------------------------------------
// Element Editor (generic for all element types)
// ---------------------------------------------------------------------------

function getElements() {
  if (!currentElementType) return [];
  if (!caseData.system[currentElementType]) caseData.system[currentElementType] = [];
  return caseData.system[currentElementType];
}

function addElement() {
  if (!currentElementType || !schemas[currentElementType]) return;
  const schema = schemas[currentElementType];
  const elements = getElements();
  const uid = elements.length ? Math.max(...elements.map((e) => e.uid || 0)) + 1 : 1;

  const newEl = {};
  for (const field of schema.fields) {
    if (field.name === "uid") newEl.uid = uid;
    else if (field.required && field.type === "string") newEl[field.name] = "";
    else newEl[field.name] = null;
  }
  elements.push(newEl);
  renderCurrentElementView();
  updateBadges();
}

function removeElement(i) {
  getElements().splice(i, 1);
  renderCurrentElementView();
  updateBadges();
}

function updateElementField(i, field, value, fieldType) {
  const elements = getElements();
  if (fieldType === "integer") {
    elements[i][field] = value ? parseInt(value, 10) : null;
  } else if (fieldType === "number") {
    elements[i][field] = value ? Number(value) : null;
  } else if (fieldType === "boolean") {
    elements[i][field] = value === "" ? null : value === "true";
  } else if (fieldType === "number_or_file") {
    if (value === "" || value === null) {
      elements[i][field] = null;
    } else {
      const num = Number(value);
      elements[i][field] = isNaN(num) ? value : num;
    }
  } else {
    elements[i][field] = value || null;
  }
}

function renderElements() {
  const container = document.getElementById("elementList");
  container.innerHTML = "";
  const elements = getElements();
  const schema = schemas[currentElementType];
  if (!schema) return;

  elements.forEach((el, i) => {
    const card = document.createElement("div");
    card.className = "element-card";

    const headerHtml = `
      <div class="element-card-header">
        <strong>${schema.label} #${el.uid || i + 1}${el.name ? " — " + escapeHtml(String(el.name)) : ""}</strong>
        <button class="btn-danger" onclick="removeElement(${i})">Remove</button>
      </div>`;

    let fieldsHtml = '<div class="form-grid">';
    for (const field of schema.fields) {
      const val = el[field.name];
      const displayVal = val != null ? val : "";

      if (field.type === "boolean") {
        fieldsHtml += `<label>${field.name}
          <select onchange="updateElementField(${i},'${field.name}',this.value,'boolean')">
            <option value="" ${val == null ? "selected" : ""}>—</option>
            <option value="true" ${val === true ? "selected" : ""}>Yes</option>
            <option value="false" ${val === false ? "selected" : ""}>No</option>
          </select></label>`;
      } else if (field.type === "integer") {
        fieldsHtml += `<label>${field.name}${field.required ? " *" : ""}
          <input type="number" value="${displayVal}" onchange="updateElementField(${i},'${field.name}',this.value,'integer')"></label>`;
      } else if (field.type === "number") {
        fieldsHtml += `<label>${field.name}${field.required ? " *" : ""}
          <input type="number" step="any" value="${displayVal}" onchange="updateElementField(${i},'${field.name}',this.value,'number')"></label>`;
      } else if (field.type === "number_or_file") {
        fieldsHtml += `<label>${field.name}${field.required ? " *" : ""}
          <input type="text" value="${displayVal}" placeholder="number or filename"
            onchange="updateElementField(${i},'${field.name}',this.value,'number_or_file')"></label>`;
      } else {
        // string or ref
        fieldsHtml += `<label>${field.name}${field.required ? " *" : ""}
          <input type="text" value="${escapeHtml(String(displayVal))}"
            onchange="updateElementField(${i},'${field.name}',this.value,'string')"></label>`;
      }
    }
    fieldsHtml += "</div>";

    card.innerHTML = headerHtml + fieldsHtml;
    container.appendChild(card);
  });
}

// ---------------------------------------------------------------------------
// View toggle & Spreadsheet view
// ---------------------------------------------------------------------------

function setElementView(view) {
  currentElementView = view;
  document.getElementById("btnCardView").classList.toggle("active", view === "card");
  document.getElementById("btnSpreadsheetView").classList.toggle("active", view === "spreadsheet");
  renderCurrentElementView();
}

function renderCurrentElementView() {
  if (currentElementView === "spreadsheet") {
    document.getElementById("elementList").style.display = "none";
    document.getElementById("spreadsheetContainer").style.display = "block";
    renderSpreadsheet();
  } else {
    document.getElementById("elementList").style.display = "block";
    document.getElementById("spreadsheetContainer").style.display = "none";
    destroySpreadsheet();
    renderElements();
  }
}

function destroySpreadsheet() {
  if (spreadsheetInstance) {
    var container = document.getElementById("spreadsheetContainer");
    if (container && typeof jspreadsheet !== "undefined" && spreadsheetInstance.destroy) {
      spreadsheetInstance.destroy();
    }
    spreadsheetInstance = null;
    document.getElementById("spreadsheetContainer").innerHTML = "";
  }
}

function renderSpreadsheet(retryCount) {
  destroySpreadsheet();
  var container = document.getElementById("spreadsheetContainer");
  container.innerHTML = "";

  if (typeof jspreadsheet === "undefined") {
    var attempt = retryCount || 0;
    if (attempt < 5) {
      container.innerHTML = '<p class="help-text" style="padding:16px">Spreadsheet library is loading… Retrying (' + (attempt + 1) + '/5)…</p>';
      setTimeout(function () { renderSpreadsheet(attempt + 1); }, 1000);
    } else {
      container.innerHTML = '<p class="help-text" style="padding:16px">Spreadsheet library failed to load. '
        + 'Check your network connection or try reloading the page.</p>';
    }
    return;
  }

  var elements = getElements();
  var schema = schemas[currentElementType];
  if (!schema) return;

  var columns = schema.fields.map(function (field) {
    var col = { title: field.name, width: 120 };
    if (field.type === "boolean") {
      col.type = "dropdown";
      col.source = ["—", "true", "false"];
      col.width = 80;
    } else if (field.type === "integer") {
      col.type = "numeric";
      col.mask = "#,##0";
      col.decimal = ".";
    } else if (field.type === "number") {
      col.type = "numeric";
      col.decimal = ".";
    } else {
      col.type = "text";
    }
    return col;
  });

  // Build data rows from existing elements
  var data = elements.map(function (el) {
    return schema.fields.map(function (field) {
      var val = el[field.name];
      if (val === null || val === undefined) {
        return field.type === "boolean" ? "—" : "";
      }
      if (field.type === "boolean") return String(val);
      return val;
    });
  });

  // Ensure at least one empty row when no elements exist
  if (data.length === 0) {
    data = [schema.fields.map(function () { return ""; })];
  }

  spreadsheetInstance = jspreadsheet(container, {
    data: data,
    columns: columns,
    minDimensions: [schema.fields.length, 1],
    allowInsertRow: true,
    allowDeleteRow: true,
    allowInsertColumn: false,
    allowDeleteColumn: false,
    columnSorting: true,
    tableOverflow: true,
    tableWidth: "100%",
    tableHeight: "600px",
    onchange: function (instance, cell, colIndex, rowIndex, value) {
      syncSpreadsheetToData();
    },
    oninsertrow: function () {
      syncSpreadsheetToData();
    },
    ondeleterow: function () {
      syncSpreadsheetToData();
    },
    onpaste: function () {
      // Defer to allow jspreadsheet to finish processing the paste
      setTimeout(function () { syncSpreadsheetToData(); }, 0);
    },
  });
}

function syncSpreadsheetToData() {
  if (!spreadsheetInstance || !currentElementType) return;
  var schema = schemas[currentElementType];
  if (!schema) return;

  var tableData = spreadsheetInstance.getData();
  var newElements = [];

  for (var r = 0; r < tableData.length; r++) {
    var row = tableData[r];
    // Skip completely empty rows
    var allEmpty = row.every(function (cell) { return cell === "" || cell === null || cell === undefined; });
    if (allEmpty) continue;

    var el = {};
    for (var c = 0; c < schema.fields.length; c++) {
      var field = schema.fields[c];
      var val = row[c];

      if (val === "" || val === null || val === undefined) {
        el[field.name] = null;
      } else if (field.type === "integer") {
        el[field.name] = parseInt(val, 10);
        if (isNaN(el[field.name])) el[field.name] = null;
      } else if (field.type === "number") {
        el[field.name] = Number(val);
        if (isNaN(el[field.name])) el[field.name] = null;
      } else if (field.type === "boolean") {
        el[field.name] = val === "true" ? true : val === "false" ? false : null;
      } else if (field.type === "number_or_file") {
        var num = Number(val);
        el[field.name] = isNaN(num) ? val : num;
      } else {
        el[field.name] = val || null;
      }
    }
    newElements.push(el);
  }

  caseData.system[currentElementType] = newElements;
  updateBadges();
}

// ---------------------------------------------------------------------------
// Badges
// ---------------------------------------------------------------------------

function updateBadges() {
  for (const elemType of Object.keys(schemas)) {
    const badge = document.getElementById("badge-" + elemType);
    if (badge) {
      const count = (caseData.system[elemType] || []).length;
      badge.textContent = count;
    }
  }
}

// ---------------------------------------------------------------------------
// Case actions
// ---------------------------------------------------------------------------

function buildFullCaseData() {
  caseData.case_name = document.getElementById("caseName").value.trim() || "case";
  caseData.options = gatherOptions();
  return JSON.parse(JSON.stringify(caseData));
}

function newCase() {
  if (!confirm("Create a new empty case? Current data will be lost.")) return;
  document.getElementById("caseName").value = "my_case";
  caseData.case_name = "my_case";
  caseData.options = {};
  caseData.simulation = { block_array: [], stage_array: [], scenario_array: [] };
  caseData.system = {};
  caseData.data_files = {};

  // Reset options form
  document.querySelectorAll('[id^="opt-"]').forEach((el) => {
    if (el.tagName === "SELECT") el.selectedIndex = 0;
    else el.value = "";
  });

  updateBadges();
  switchTab("options");
}

async function downloadCase() {
  const data = buildFullCaseData();
  try {
    const resp = await fetch("/api/case/download", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    if (!resp.ok) {
      const err = await resp.json();
      alert("Error: " + (err.error || "Unknown error"));
      return;
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = (data.case_name || "case") + ".zip";
    a.click();
    URL.revokeObjectURL(url);
  } catch (e) {
    alert("Download failed: " + e.message);
  }
}

async function uploadCase(input) {
  const file = input.files[0];
  if (!file) return;
  input.value = "";

  const formData = new FormData();
  formData.append("file", file);

  try {
    const resp = await fetch("/api/case/upload", {
      method: "POST",
      body: formData,
    });
    if (!resp.ok) {
      const err = await resp.json();
      alert("Error: " + (err.error || "Unknown error"));
      return;
    }
    const data = await resp.json();

    // Load into state
    document.getElementById("caseName").value = data.case_name || "uploaded_case";
    caseData.case_name = data.case_name;
    caseData.simulation = data.simulation || { block_array: [], stage_array: [], scenario_array: [] };
    caseData.system = data.system || {};
    caseData.data_files = data.data_files || {};

    loadOptions(data.options);
    updateBadges();
    switchTab("options");
    alert("Case loaded successfully!");
  } catch (e) {
    alert("Upload failed: " + e.message);
  }
}

async function previewJSON() {
  const data = buildFullCaseData();
  try {
    const resp = await fetch("/api/case/preview", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    const json = await resp.json();
    document.getElementById("jsonPreview").textContent = JSON.stringify(json, null, 2);
    document.getElementById("jsonModal").style.display = "flex";
  } catch (e) {
    alert("Preview failed: " + e.message);
  }
}

function closeModal() {
  document.getElementById("jsonModal").style.display = "none";
}

// ---------------------------------------------------------------------------
// Solver / Webservice integration
// ---------------------------------------------------------------------------

async function saveWsConfig() {
  const url = document.getElementById("wsUrl").value.trim();
  if (!url) {
    alert("Please enter a webservice URL.");
    return;
  }
  try {
    const resp = await fetch("/api/solve/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ webservice_url: url }),
    });
    const data = await resp.json();
    if (resp.ok) {
      webserviceUrl = data.webservice_url;
      setWsStatus("ok", "Configuration saved.");
    } else {
      setWsStatus("fail", data.error || "Failed to save.");
    }
  } catch (e) {
    setWsStatus("fail", "Error: " + e.message);
  }
}

async function testWsConnection() {
  const url = document.getElementById("wsUrl").value.trim();
  if (!url) {
    setWsStatus("fail", "Please enter a URL first.");
    return;
  }
  // Save first, then test by listing jobs
  await saveWsConfig();
  setWsStatus("", "Testing connection…");
  try {
    const resp = await fetch("/api/solve/jobs");
    if (resp.ok) {
      const data = await resp.json();
      const count = (data.jobs || []).length;
      setWsStatus("ok", `Connected! ${count} job(s) found.`);
      updateWsDot(true);
      renderJobsList(data.jobs || []);
    } else {
      const data = await resp.json();
      setWsStatus("fail", data.error || "Connection failed.");
      updateWsDot(false);
    }
  } catch (e) {
    setWsStatus("fail", "Connection failed: " + e.message);
    updateWsDot(false);
  }
}

function setWsStatus(cls, msg) {
  const el = document.getElementById("wsConnectionStatus");
  el.className = "ws-connection-status" + (cls ? " " + cls : "");
  el.textContent = msg;
}

function updateWsDot(connected) {
  const dot = document.getElementById("wsStatusDot");
  if (dot) {
    dot.className = "ws-status-dot" + (connected ? " connected" : " error");
  }
}

async function solveCase() {
  const data = buildFullCaseData();

  setSolverStatus("Submitting case to webservice…");
  showSolverProgress(true, 10, "Submitting…");

  try {
    const resp = await fetch("/api/solve/submit", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    const result = await resp.json();

    if (!resp.ok) {
      showSolverProgress(true, 100, "Submission failed: " + (result.error || "Unknown error"), true);
      updateWsDot(false);
      return;
    }

    solverJobId = result.token;
    updateWsDot(true);
    showSolverProgress(true, 20, "Job submitted. Token: " + solverJobId);
    switchTab("solver");

    // Start polling
    startPolling();
  } catch (e) {
    showSolverProgress(true, 100, "Submission failed: " + e.message, true);
  }
}

function startPolling() {
  if (solverPollTimer) clearInterval(solverPollTimer);
  solverPollTimer = setInterval(pollStatus, 3000);
  // Also poll immediately
  pollStatus();
}

async function pollStatus() {
  if (!solverJobId) {
    if (solverPollTimer) clearInterval(solverPollTimer);
    return;
  }

  try {
    const resp = await fetch(`/api/solve/status/${encodeURIComponent(solverJobId)}`);
    const data = await resp.json();

    if (!resp.ok) {
      showSolverProgress(true, 100, "Status check failed: " + (data.error || ""), true);
      clearInterval(solverPollTimer);
      return;
    }

    const status = data.status;

    if (status === "pending") {
      showSolverProgress(true, 30, "Job pending…");
    } else if (status === "running") {
      showSolverProgress(true, 60, "Solver is running…");
    } else if (status === "completed") {
      clearInterval(solverPollTimer);
      solverPollTimer = null;
      showSolverProgress(true, 100, "Completed!", false, true);
      document.getElementById("solverActions").style.display = "block";
    } else if (status === "failed") {
      clearInterval(solverPollTimer);
      solverPollTimer = null;
      const errMsg = data.error || "Unknown error";
      showSolverProgress(true, 100, "Failed: " + errMsg, true);
    }
  } catch (e) {
    // Don't stop polling on transient errors
  }
}

function setSolverStatus(msg) {
  const el = document.getElementById("solverStatus");
  el.innerHTML = `<p>${escapeHtml(msg)}</p>`;
}

function showSolverProgress(show, percent, text, isError, isComplete) {
  const container = document.getElementById("solverProgress");
  const fill = document.getElementById("progressFill");
  const label = document.getElementById("solverStatusText");

  container.style.display = show ? "block" : "none";
  fill.style.width = percent + "%";
  fill.className = "progress-fill" + (isComplete ? " complete" : "") + (isError ? " error" : "");
  label.textContent = text || "";

  if (isError || isComplete) {
    setSolverStatus(text);
  }
}

async function loadSolverResults() {
  if (!solverJobId) {
    alert("No completed job to load.");
    return;
  }

  try {
    const resp = await fetch(`/api/solve/results/${encodeURIComponent(solverJobId)}`);
    if (!resp.ok) {
      const data = await resp.json();
      alert("Failed to load results: " + (data.error || "Unknown error"));
      return;
    }
    resultsData = await resp.json();
    switchTab("results");
    renderResults();
  } catch (e) {
    alert("Failed to load results: " + e.message);
  }
}

async function refreshJobsList() {
  try {
    const resp = await fetch("/api/solve/jobs");
    if (resp.ok) {
      const data = await resp.json();
      renderJobsList(data.jobs || []);
      updateWsDot(true);
    } else {
      const data = await resp.json();
      alert(data.error || "Failed to list jobs.");
      updateWsDot(false);
    }
  } catch (e) {
    alert("Failed to list jobs: " + e.message);
  }
}

function renderJobsList(jobs) {
  const tbody = document.querySelector("#jobsTable tbody");
  tbody.innerHTML = "";

  if (!jobs.length) {
    const tr = document.createElement("tr");
    tr.innerHTML = '<td colspan="5" class="help-text">No jobs found.</td>';
    tbody.appendChild(tr);
    return;
  }

  for (const job of jobs) {
    const tr = document.createElement("tr");
    const shortToken = job.token ? job.token.substring(0, 8) + "…" : "";
    const created = job.createdAt ? new Date(job.createdAt).toLocaleString() : "";
    const statusClass = job.status === "completed" ? "status-ok" :
                        job.status === "failed" ? "status-fail" :
                        job.status === "running" ? "status-running" : "";

    const tdToken = document.createElement("td");
    tdToken.title = job.token || "";
    tdToken.textContent = shortToken;

    const tdFile = document.createElement("td");
    tdFile.textContent = job.systemFile || "";

    const tdStatus = document.createElement("td");
    const statusSpan = document.createElement("span");
    statusSpan.className = "status-badge " + statusClass;
    statusSpan.textContent = job.status || "";
    tdStatus.appendChild(statusSpan);

    const tdCreated = document.createElement("td");
    tdCreated.textContent = created;

    const tdActions = document.createElement("td");
    if (job.status === "completed" && job.token) {
      const btn = document.createElement("button");
      btn.className = "btn-sm";
      btn.textContent = "Load Results";
      btn.addEventListener("click", () => loadJobResults(job.token));
      tdActions.appendChild(btn);
    }
    if ((job.status === "completed" || job.status === "failed") && job.token) {
      const logBtn = document.createElement("button");
      logBtn.className = "btn-sm";
      logBtn.textContent = "View Logs";
      logBtn.style.marginLeft = "4px";
      logBtn.addEventListener("click", () => viewJobLogs(job.token));
      tdActions.appendChild(logBtn);
    }

    tr.appendChild(tdToken);
    tr.appendChild(tdFile);
    tr.appendChild(tdStatus);
    tr.appendChild(tdCreated);
    tr.appendChild(tdActions);
    tbody.appendChild(tr);
  }
}

async function loadJobResults(token) {
  try {
    const safeToken = encodeURIComponent(token);
    const resp = await fetch(`/api/solve/results/${safeToken}`);
    if (!resp.ok) {
      const data = await resp.json();
      alert("Failed to load results: " + (data.error || "Unknown error"));
      return;
    }
    resultsData = await resp.json();
    switchTab("results");
    renderResults();
  } catch (e) {
    alert("Failed to load results: " + e.message);
  }
}

async function viewJobLogs(token) {
  try {
    var safeToken = encodeURIComponent(token);
    var resp = await fetch("/api/solve/job_logs/" + safeToken);
    if (!resp.ok) {
      var errData = await resp.json().catch(function () { return {}; });
      alert("Failed to load job logs: " + (errData.error || "Unknown error"));
      return;
    }
    var data = await resp.json();
    var text = "";
    if (data.stdout) { text += "=== stdout ===\n" + data.stdout + "\n"; }
    if (data.stderr) { text += "=== stderr ===\n" + data.stderr + "\n"; }
    if (!text) { text = "No terminal output available for this job."; }
    // Update terminal output in the results view; preserve existing data
    if (!resultsData) {
      resultsData = { solution: {}, outputs: {}, terminal_output: "" };
    }
    resultsData.terminal_output = text;
    switchTab("results");
    renderResults();
  } catch (e) {
    alert("Failed to load job logs: " + e.message);
  }
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

async function uploadResults(input) {
  const file = input.files[0];
  if (!file) return;
  input.value = "";

  const formData = new FormData();
  formData.append("file", file);

  try {
    const resp = await fetch("/api/results/upload", {
      method: "POST",
      body: formData,
    });
    if (!resp.ok) {
      const err = await resp.json();
      alert("Error: " + (err.error || "Unknown error"));
      return;
    }
    resultsData = await resp.json();
    switchTab("results");
    renderResults();
  } catch (e) {
    alert("Upload failed: " + e.message);
  }
}

function renderResults() {
  if (!resultsData) return;

  // Solution summary
  const solDiv = document.getElementById("resultsSolution");
  const solBody = document.querySelector("#solutionTable tbody");
  solBody.innerHTML = "";

  if (Object.keys(resultsData.solution).length) {
    solDiv.style.display = "block";
    for (const [k, v] of Object.entries(resultsData.solution)) {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${escapeHtml(k)}</td><td>${escapeHtml(String(v))}</td>`;
      solBody.appendChild(tr);
    }
  }

  // Terminal output
  var termDiv = document.getElementById("resultsTerminalOutput");
  var termContent = document.getElementById("terminalOutputContent");
  if (resultsData.terminal_output) {
    termDiv.style.display = "block";
    termContent.textContent = resultsData.terminal_output;
  } else {
    termDiv.style.display = "none";
    termContent.textContent = "";
  }

  // Outputs selector
  const outputsDiv = document.getElementById("resultsOutputs");
  const selector = document.getElementById("resultsSelector");
  selector.innerHTML = '<option value="">Select output…</option>';

  if (Object.keys(resultsData.outputs).length) {
    outputsDiv.style.display = "block";
    for (const key of Object.keys(resultsData.outputs).sort()) {
      const opt = document.createElement("option");
      opt.value = key;
      opt.textContent = key;
      selector.appendChild(opt);
    }
  }
}

function toggleTerminalOutput() {
  var el = document.getElementById("terminalOutputContent");
  el.style.display = el.style.display === "none" ? "block" : "none";
}

function resolveUidLabel(col, resultKey) {
  const uidMatch = col.match(/^uid:(\d+)$/);
  if (!uidMatch) return col;
  const uid = Number(uidMatch[1]);
  const segment = (resultKey || "").split("/")[0].toLowerCase();
  const arr = caseData?.system?.[segment];
  if (Array.isArray(arr)) {
    const found = arr.find((e) => e.uid === uid);
    if (found?.name) return found.name;
  }
  return col;
}

const _META_COLS = new Set(["scenario", "stage", "block"]);

function showResultsTable() {
  const key = document.getElementById("resultsSelector").value;
  const container = document.getElementById("resultsTableContainer");
  container.innerHTML = "";

  if (!key || !resultsData || !resultsData.outputs[key]) {
    _renderColumnSelector();
    _renderResultChart();
    return;
  }

  // Reinitialize _selectedColumns when a different file is selected
  if (key !== _resultsCurrentKey) {
    _resultsCurrentKey = key;
    const data = resultsData.outputs[key];
    _selectedColumns = new Set(
      data.columns.filter((c) => !_META_COLS.has(c))
    );
  }

  const data = resultsData.outputs[key];
  const table = document.createElement("table");
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const col of data.columns) {
    const th = document.createElement("th");
    th.textContent = resolveUidLabel(col, key);
    if (_META_COLS.has(col)) {
      th.className = "col-meta";
    } else {
      th.className = _selectedColumns.has(col) ? "col-selected" : "";
      th.onclick = () => toggleResultColumn(col);
    }
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const row of data.data) {
    const tr = document.createElement("tr");
    for (const cell of row) {
      const td = document.createElement("td");
      td.textContent = cell;
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  container.appendChild(table);

  _renderColumnSelector();
  _renderResultChart();
}

function _renderColumnSelector() {
  const key = document.getElementById("resultsSelector").value;
  const bar = document.getElementById("resultsColumnSelector");
  bar.innerHTML = "";

  if (!key || !resultsData || !resultsData.outputs[key]) {
    bar.style.display = "none";
    return;
  }

  const data = resultsData.outputs[key];
  const dataCols = data.columns.filter((c) => !_META_COLS.has(c));
  if (dataCols.length === 0) { bar.style.display = "none"; return; }

  bar.style.display = "flex";
  for (const col of dataCols) {
    const chip = document.createElement("span");
    chip.className = "col-chip" + (_selectedColumns.has(col) ? " col-chip-selected" : "");
    chip.textContent = resolveUidLabel(col, key);
    chip.onclick = () => toggleResultColumn(col);
    bar.appendChild(chip);
  }
}

function toggleResultColumn(col) {
  if (_selectedColumns.has(col)) {
    _selectedColumns.delete(col);
  } else {
    _selectedColumns.add(col);
  }
  _renderColumnSelector();
  _refreshResultHeaders();
  _renderResultChart();
}

function _refreshResultHeaders() {
  const ths = document.querySelectorAll("#resultsTableContainer thead tr th");
  const key = document.getElementById("resultsSelector").value;
  const data = key && resultsData?.outputs?.[key];
  if (!data) return;
  data.columns.forEach((col, i) => {
    if (i >= ths.length) return;
    if (!_META_COLS.has(col)) {
      ths[i].className = _selectedColumns.has(col) ? "col-selected" : "";
    }
  });
}

function selectAllResultColumns() {
  const key = document.getElementById("resultsSelector").value;
  if (!key || !resultsData?.outputs?.[key]) return;
  const data = resultsData.outputs[key];
  _selectedColumns = new Set(data.columns.filter((c) => !_META_COLS.has(c)));
  _renderColumnSelector();
  _refreshResultHeaders();
  _renderResultChart();
}

function deselectAllResultColumns() {
  _selectedColumns = new Set();
  _renderColumnSelector();
  _refreshResultHeaders();
  _renderResultChart();
}

function _renderResultChart() {
  const key = document.getElementById("resultsSelector").value;
  const canvas = document.getElementById("resultsChart");

  if (!key || !resultsData || !resultsData.outputs[key] || _selectedColumns.size === 0) {
    if (resultsChart) { resultsChart.destroy(); resultsChart = null; }
    canvas.style.display = "none";
    document.getElementById("resetZoomBtn").style.display = "none";
    return;
  }

  const data = resultsData.outputs[key];
  canvas.style.display = "block";

  if (resultsChart) resultsChart.destroy();

  // Build labels from scenario/stage/block
  const labels = data.data.map((row) => {
    const parts = [];
    if (data.columns.includes("scenario")) parts.push("S" + row[data.columns.indexOf("scenario")]);
    if (data.columns.includes("stage")) parts.push("T" + row[data.columns.indexOf("stage")]);
    if (data.columns.includes("block")) parts.push("B" + row[data.columns.indexOf("block")]);
    return parts.join("-");
  });

  // Build datasets for selected columns only
  const datasets = [];
  const colors = [
    "#2563eb", "#dc2626", "#16a34a", "#ea580c", "#7c3aed",
    "#0891b2", "#be185d", "#65a30d", "#c2410c", "#4f46e5",
  ];
  let colorIdx = 0;

  for (let c = 0; c < data.columns.length; c++) {
    const col = data.columns[c];
    if (_META_COLS.has(col)) continue;
    if (!_selectedColumns.has(col)) continue;
    datasets.push({
      label: resolveUidLabel(col, key),
      data: data.data.map((row) => Number(row[c])),
      borderColor: colors[colorIdx % colors.length],
      backgroundColor: colors[colorIdx % colors.length] + "33",
      fill: false,
      tension: 0,
    });
    colorIdx++;
  }

  const enableZoom = labels.length > 50;
  const zoomBtn = document.getElementById("resetZoomBtn");
  zoomBtn.style.display = enableZoom ? "inline-block" : "none";

  resultsChart = new Chart(canvas, {
    type: "line",
    data: { labels, datasets },
    options: {
      responsive: true,
      plugins: {
        title: { display: true, text: key },
        zoom: enableZoom ? {
          pan: {
            enabled: true,
            mode: "x",
          },
          zoom: {
            wheel: { enabled: true },
            pinch: { enabled: true },
            mode: "x",
          },
        } : false,
      },
      onClick: function (e) {
        handleChartLeftClick(e);
      },
      scales: {
        y: { beginAtZero: true },
      },
    },
  });

  // Right-click on chart canvas opens the line settings popup
  canvas.removeEventListener("contextmenu", handleChartContextMenu);
  canvas.addEventListener("contextmenu", handleChartContextMenu);
}

// Alias kept for backwards compatibility
function showResultsChart() {
  _renderResultChart();
}

function resetChartZoom() {
  if (resultsChart) resultsChart.resetZoom();
}

function handleChartContextMenu(e) {
  e.preventDefault();
  if (!resultsChart) return;
  // Find the nearest dataset element under the pointer
  const points = resultsChart.getElementsAtEventForMode(
    e, "nearest", { intersect: false }, false
  );
  if (points.length > 0) {
    openChartLinePopup(points[0].datasetIndex, e);
  }
}

function handleChartLeftClick(e) {
  if (!resultsChart) return;
  const points = resultsChart.getElementsAtEventForMode(
    e.native, "nearest", { intersect: false }, false
  );
  if (points.length === 0) {
    closeChartDataPopup();
    return;
  }
  const pt = points[0];
  const ds = resultsChart.data.datasets[pt.datasetIndex];
  const label = resultsChart.data.labels[pt.index];
  const value = ds.data[pt.index];

  const popup = document.getElementById("chartDataPopup");
  document.getElementById("chartDataPopupTitle").textContent = ds.label;
  const body = document.getElementById("chartDataPopupBody");
  body.innerHTML = "";

  // Show clicked point prominently
  const mainRow = document.createElement("div");
  mainRow.className = "chart-data-highlight";
  mainRow.textContent = label + ": " + value;
  body.appendChild(mainRow);

  // Show all values for this dataset
  const table = document.createElement("table");
  table.className = "chart-data-table";
  const thead = document.createElement("thead");
  const hr = document.createElement("tr");
  const th1 = document.createElement("th"); th1.textContent = "Point";
  const th2 = document.createElement("th"); th2.textContent = "Value";
  hr.appendChild(th1); hr.appendChild(th2);
  thead.appendChild(hr); table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (let i = 0; i < ds.data.length; i++) {
    const tr = document.createElement("tr");
    if (i === pt.index) tr.className = "chart-data-active-row";
    const td1 = document.createElement("td");
    td1.textContent = resultsChart.data.labels[i];
    const td2 = document.createElement("td");
    td2.textContent = ds.data[i];
    tr.appendChild(td1); tr.appendChild(td2);
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  body.appendChild(table);

  popup.style.display = "block";
  const container = popup.parentElement;
  const rect = container.getBoundingClientRect();
  popup.style.left = (e.native.clientX - rect.left + 4) + "px";
  popup.style.top = (e.native.clientY - rect.top + 4) + "px";
}

function closeChartDataPopup() {
  document.getElementById("chartDataPopup").style.display = "none";
}

function openChartLinePopup(datasetIndex, mouseEvent) {
  if (!resultsChart) return;
  const ds = resultsChart.data.datasets[datasetIndex];
  if (!ds) return;
  chartLinePopupIndex = datasetIndex;

  const popup = document.getElementById("chartLinePopup");
  document.getElementById("chartLinePopupTitle").textContent = ds.label;
  document.getElementById("clpColor").value = ds.borderColor.slice(0, 7);
  document.getElementById("clpWidth").value = String(ds.borderWidth || 2);
  document.getElementById("clpDash").value = (ds.borderDash || []).join(",");
  const ps = ds.pointStyle === false ? "false" : (ds.pointStyle || "circle");
  document.getElementById("clpPoint").value = ps;
  document.getElementById("clpPointSize").value = String(ds.pointRadius || 3);

  popup.style.display = "block";
  const container = popup.parentElement;
  const rect = container.getBoundingClientRect();
  popup.style.left = (mouseEvent.clientX - rect.left + 4) + "px";
  popup.style.top = (mouseEvent.clientY - rect.top + 4) + "px";
}

function closeChartLinePopup() {
  document.getElementById("chartLinePopup").style.display = "none";
  chartLinePopupIndex = -1;
}

function applyChartLineSettings() {
  if (!resultsChart || chartLinePopupIndex < 0) return;
  const ds = resultsChart.data.datasets[chartLinePopupIndex];
  if (!ds) return;

  const color = document.getElementById("clpColor").value;
  ds.borderColor = color;
  ds.backgroundColor = color + "33";

  ds.borderWidth = Number(document.getElementById("clpWidth").value);

  const dashVal = document.getElementById("clpDash").value;
  ds.borderDash = dashVal ? dashVal.split(",").map(Number) : [];

  const pointVal = document.getElementById("clpPoint").value;
  ds.pointStyle = pointVal === "false" ? false : pointVal;

  ds.pointRadius = Number(document.getElementById("clpPointSize").value);

  resultsChart.update();
  closeChartLinePopup();
}

async function refreshLogs() {
  const output = document.getElementById("logsOutput");
  if (!output) return;
  output.textContent = "Loading logs…";
  try {
    const resp = await fetch("/api/logs?lines=300");
    if (!resp.ok) {
      output.textContent = "Failed to load logs.";
      return;
    }
    const data = await resp.json();
    const lines = data.logs || [];
    output.textContent = lines.length ? lines.join("\n") : "No logs available yet.";
  } catch (e) {
    output.textContent = "Failed to load logs: " + e.message;
  }
}

async function refreshWebserviceLogs() {
  const output = document.getElementById("wsLogsOutput");
  if (!output) return;
  output.textContent = "Loading webservice logs…";
  try {
    const resp = await fetch("/api/solve/logs?lines=300");
    if (!resp.ok) {
      const data = await resp.json().catch(function () { return {}; });
      output.textContent = "Failed to load webservice logs: " + (data.error || resp.statusText);
      return;
    }
    const data = await resp.json();
    const lines = data.lines || [];
    const logFile = data.log_file || "";
    var header = logFile ? "Log file: " + logFile + "\n\n" : "";
    output.textContent = lines.length ? header + lines.join("\n") : "No webservice logs available yet.";
  } catch (e) {
    output.textContent = "Failed to load webservice logs: " + e.message;
  }
}

async function pingWebservice() {
  const url = document.getElementById("wsUrl").value.trim();
  if (url) {
    await saveWsConfig();
  }
  setWsStatus("", "Pinging webservice…");
  try {
    const resp = await fetch("/api/solve/ping");
    if (!resp.ok) {
      const data = await resp.json().catch(function () { return {}; });
      setWsStatus("fail", data.error || "Ping failed.");
      updateWsDot(false);
      document.getElementById("wsPingInfo").style.display = "none";
      return;
    }
    const data = await resp.json();
    setWsStatus("ok", "Webservice is running (" + data.service + ")");
    updateWsDot(true);
    document.getElementById("wsPingInfo").style.display = "block";
    document.getElementById("pingGtoptBin").textContent = data.gtopt_bin || "—";
    document.getElementById("pingGtoptVersion").textContent = data.gtopt_version || "—";
    document.getElementById("pingLogFile").textContent = data.log_file || "(console only)";
  } catch (e) {
    setWsStatus("fail", "Ping failed: " + e.message);
    updateWsDot(false);
    document.getElementById("wsPingInfo").style.display = "none";
  }
}

// ---------------------------------------------------------------------------
// Check Server – aggregated health check
// ---------------------------------------------------------------------------

async function checkServer() {
  switchTab("solver");
  setWsStatus("", "Checking server…");
  try {
    const resp = await fetch("/api/check_server");
    if (!resp.ok) {
      setWsStatus("fail", "Check server request failed.");
      updateWsDot(false);
      return;
    }
    const data = await resp.json();
    const parts = [];

    // Ping
    if (data.ping && data.ping.status === "ok") {
      const p = data.ping.data || {};
      parts.push("✅ Ping: " + (p.service || "ok") + (p.gtopt_version ? " (gtopt " + p.gtopt_version + ")" : ""));
      updateWsDot(true);
      document.getElementById("wsPingInfo").style.display = "block";
      document.getElementById("pingGtoptBin").textContent = (p.gtopt_bin || "—");
      document.getElementById("pingGtoptVersion").textContent = (p.gtopt_version || "—");
      document.getElementById("pingLogFile").textContent = (p.log_file || "(console only)");
    } else {
      parts.push("❌ Ping: " + (data.ping ? data.ping.error : "unavailable"));
      updateWsDot(false);
    }

    // Logs
    if (data.logs && data.logs.status === "ok") {
      const l = data.logs.data || {};
      const count = (l.lines || []).length;
      parts.push("✅ Logs: " + count + " line(s) available");
      // Also update the webservice logs panel
      const wsOutput = document.getElementById("wsLogsOutput");
      if (wsOutput && count > 0) {
        const header = l.log_file ? "Log file: " + l.log_file + "\n\n" : "";
        wsOutput.textContent = header + l.lines.join("\n");
      }
    } else {
      parts.push("❌ Logs: " + (data.logs ? data.logs.error : "unavailable"));
    }

    // Jobs
    if (data.jobs && data.jobs.status === "ok") {
      const j = data.jobs.data || {};
      const count = (j.jobs || []).length;
      parts.push("✅ Jobs: " + count + " job(s) found");
      renderJobsList(j.jobs || []);
    } else {
      parts.push("❌ Jobs: " + (data.jobs ? data.jobs.error : "unavailable"));
    }

    const allOk = (data.ping && data.ping.status === "ok") &&
                (data.logs && data.logs.status === "ok") &&
                (data.jobs && data.jobs.status === "ok");
    setWsStatus(allOk ? "ok" : "fail", parts.join("  |  "));
  } catch (e) {
    setWsStatus("fail", "Check server failed: " + e.message);
    updateWsDot(false);
  }
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

async function shutdownService() {
  if (!confirm("Are you sure you want to shut down the gtopt GUI and all services?")) {
    return;
  }
  try {
    await fetch("/api/shutdown", { method: "POST" });
  } catch (e) {
    // Connection will likely be lost immediately – that's expected.
  }
  document.body.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100vh;font-family:sans-serif;color:#64748b"><h2>gtopt GUI has been shut down.</h2></div>';
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function escapeHtml(str) {
  const div = document.createElement("div");
  div.textContent = str;
  return div.innerHTML;
}

// ---------------------------------------------------------------------------
// Quick Assistant – move, minimize, close
// ---------------------------------------------------------------------------

function closeAssistant() {
  const card = document.getElementById("assistantCard");
  if (card) card.style.display = "none";
}

function minimizeAssistant() {
  const card = document.getElementById("assistantCard");
  if (!card) return;
  card.classList.toggle("minimized");
}

/* Drag-to-move */
(function initAssistantDrag() {
  let isDragging = false;
  let offsetX = 0;
  let offsetY = 0;

  function onPointerDown(e) {
    const card = document.getElementById("assistantCard");
    if (!card) return;
    isDragging = true;
    const rect = card.getBoundingClientRect();
    offsetX = e.clientX - rect.left;
    offsetY = e.clientY - rect.top;
    card.style.transition = "none";
    e.preventDefault();
  }

  function onPointerMove(e) {
    if (!isDragging) return;
    const card = document.getElementById("assistantCard");
    if (!card) return;
    const x = Math.min(window.innerWidth - card.offsetWidth, Math.max(0, e.clientX - offsetX));
    const y = Math.min(window.innerHeight - card.offsetHeight, Math.max(0, e.clientY - offsetY));
    card.style.left = x + "px";
    card.style.top = y + "px";
    card.style.right = "auto";
    card.style.bottom = "auto";
  }

  function onPointerUp() {
    if (!isDragging) return;
    isDragging = false;
    const card = document.getElementById("assistantCard");
    if (card) card.style.transition = "";
  }

  document.addEventListener("DOMContentLoaded", function () {
    const header = document.getElementById("assistantHeader");
    if (header) {
      header.addEventListener("pointerdown", onPointerDown);
    }
    document.addEventListener("pointermove", onPointerMove);
    document.addEventListener("pointerup", onPointerUp);
  });
})();

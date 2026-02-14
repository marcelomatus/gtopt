/* gtopt GUI Service – Main Application Logic */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let schemas = {};
let currentTab = "options";
let currentElementType = null;
let resultsData = null;
let resultsChart = null;

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

document.addEventListener("DOMContentLoaded", async () => {
  const resp = await fetch("/api/schemas");
  schemas = await resp.json();

  // Set up nav clicks
  document.querySelectorAll(".nav-link").forEach((link) => {
    link.addEventListener("click", (e) => {
      e.preventDefault();
      switchTab(link.dataset.tab);
    });
  });

  updateBadges();
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
  } else if (schemas[tab]) {
    currentElementType = tab;
    document.getElementById("panel-element").style.display = "block";
    document.getElementById("panel-element").classList.add("active");
    document.getElementById("elementTitle").textContent = schemas[tab].label + "s";
    document.getElementById("addElementBtn").textContent = "+ Add " + schemas[tab].label;
    renderElements();
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
  ];
  const boolFields = ["use_line_losses", "use_kirchhoff", "use_single_bus", "use_lp_names"];

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
    caseData.simulation.stage_array[i][field] = Number(value);
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
  } else {
    caseData.simulation.scenario_array[i][field] = Number(value);
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
  renderElements();
  updateBadges();
}

function removeElement(i) {
  getElements().splice(i, 1);
  renderElements();
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

function showResultsTable() {
  const key = document.getElementById("resultsSelector").value;
  const container = document.getElementById("resultsTableContainer");
  container.innerHTML = "";

  if (!key || !resultsData || !resultsData.outputs[key]) return;

  const data = resultsData.outputs[key];
  const table = document.createElement("table");
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const col of data.columns) {
    const th = document.createElement("th");
    th.textContent = col;
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
}

function showResultsChart() {
  const key = document.getElementById("resultsSelector").value;
  if (!key || !resultsData || !resultsData.outputs[key]) {
    alert("Please select an output first.");
    return;
  }

  const data = resultsData.outputs[key];
  const canvas = document.getElementById("resultsChart");
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

  // Build datasets for uid columns
  const datasets = [];
  const colors = [
    "#2563eb", "#dc2626", "#16a34a", "#ea580c", "#7c3aed",
    "#0891b2", "#be185d", "#65a30d", "#c2410c", "#4f46e5",
  ];
  let colorIdx = 0;

  for (let c = 0; c < data.columns.length; c++) {
    const col = data.columns[c];
    if (["scenario", "stage", "block"].includes(col)) continue;
    datasets.push({
      label: col,
      data: data.data.map((row) => Number(row[c])),
      borderColor: colors[colorIdx % colors.length],
      backgroundColor: colors[colorIdx % colors.length] + "33",
      fill: false,
      tension: 0.3,
    });
    colorIdx++;
  }

  resultsChart = new Chart(canvas, {
    type: "line",
    data: { labels, datasets },
    options: {
      responsive: true,
      plugins: {
        title: { display: true, text: key },
      },
      scales: {
        y: { beginAtZero: true },
      },
    },
  });
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function escapeHtml(str) {
  const div = document.createElement("div");
  div.textContent = str;
  return div.innerHTML;
}

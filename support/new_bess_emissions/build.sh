#!/usr/bin/env bash
# Build the new_bess_emissions support case end-to-end.
#
# Steps:
#   1.  Convert the latest PLEXOS bundle (PLEXOS20260517.zip) with
#       plexos2gtopt + the CEN-Chile emissions overlay.  Output goes
#       to ./reference/PLEXOS20260517.json plus per-class Parquet
#       inputs.  This is the *source* of heat-rate / fuel / emission
#       data for the PLP overlay step below.
#
#   2.  Convert the PLP 2-year case with plp2gtopt to a base gtopt
#       JSON (./gtopt_plp_plexos_2_years.json).  This pass runs
#       *without* --plexos-overlay because that flag isn't yet wired
#       through argparse; the overlay merge happens explicitly in
#       step 3.
#
#   3.  Apply the PLEXOS-derived overlay onto the PLP-base gtopt JSON
#       in-place (heat_rate + fuel attachment + emission_array +
#       fuel.emission_factors).  Driven by
#       plp2gtopt._plexos_overlay.apply_plexos_overlay().
#
#   4.  Add the BESS overlay file (bess_battery_array.json) — kept as a
#       separate file so the case is self-documenting.  UIDs are
#       renumbered to start above the PLP-merged max UID, and bus
#       references are resolved from string names to integer uids so
#       the per-Battery `bus` field follows the same SingleId
#       convention as the PLP-side batteries.
#
# The final case directory ships:
#   * gtopt_plp_plexos_2_years.json  — PLP base + PLEXOS overlay
#   * bess_battery_array.json        — the 13 new BESS units
#   * input/                         — per-class Parquet time-series
#   * reference/                     — full PLEXOS conversion (for
#                                      manual mining; NOT auto-merged)
#   * plp_overlay_report.json        — what the PLEXOS overlay touched
#   * run.sh                         — gtopt invocation chaining all
#                                      planning files via repeated -s
set -euo pipefail
CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${CASE_DIR}/../.." && pwd)"
PLEXOS_BUNDLE="${HOME}/.cache/gtopt/cen2gtopt/pcp_archive/PCP/PLEXOS20260517.zip"
PLP_CASE_DIR="${REPO_ROOT}/support/plp/2_years"
EMISSIONS_FILE="${REPO_ROOT}/share/gtopt/emissions/cen_chile.json"
BESS_SRC="${HOME}/tmp/bess_gtopt_battery_array_plp_buses.json"

echo "=== Step 1: convert PLEXOS bundle → reference/PLEXOS20260517.json ==="
plexos2gtopt -i "${PLEXOS_BUNDLE}" -o "${CASE_DIR}/reference" \
  --emissions --emissions-file "${EMISSIONS_FILE}"

PLEXOS_JSON="${CASE_DIR}/reference/PLEXOS20260517.json"
test -f "${PLEXOS_JSON}" || { echo "ERROR: ${PLEXOS_JSON} not produced"; exit 1; }

echo "=== Step 2: convert PLP 2-year case → gtopt_plp_plexos_2_years.json ==="
plp2gtopt -i "${PLP_CASE_DIR}" -o "${CASE_DIR}/gtopt_plp_plexos_2_years"

PLP_JSON="${CASE_DIR}/gtopt_plp_plexos_2_years/gtopt_plp_plexos_2_years.json"
test -f "${PLP_JSON}" || { echo "ERROR: ${PLP_JSON} not produced"; exit 1; }

echo "=== Step 3: apply PLEXOS overlay + cen_chile.json fallback onto the PLP-base JSON ==="
python3 - <<PYEOF
import json
from pathlib import Path
from plp2gtopt._plexos_overlay import apply_plexos_overlay

plp_path    = Path("${PLP_JSON}")
plexos_path = Path("${PLEXOS_JSON}")
report_path = Path("${CASE_DIR}/plp_overlay_report.json")
emissions_path = Path("${EMISSIONS_FILE}")

with open(plp_path, "r", encoding="utf-8") as f:
    planning = json.load(f)

# Overlay carries PLEXOS HR / Fuel / per-fuel emission_factors;
# emissions_fallback_path carries per-Generator overrides for cogen /
# geothermal / waste-heat units PLEXOS records as HR=0 (PAS_MEJILLONES,
# CMPC_BUCALEMU_2, CERRO_DOMINADOR_CS, CERRO_PABELLON_U{1,2,3}).
report = apply_plexos_overlay(
    planning,
    plexos_path,
    report_path=report_path,
    emissions_fallback_path=emissions_path,
)
print(f"  matched:  {len(report.matched)} generators")
print(f"  fuels:    {len(report.fuels_added)} added, {len(report.fuels_reused)} reused")
print(f"  emissions carried: {report.emissions_carried}")

with open(plp_path, "w", encoding="utf-8") as f:
    json.dump(planning, f, indent=2, ensure_ascii=False, sort_keys=False)
print(f"  written:  {plp_path}")
PYEOF

echo "=== Step 4: build bess_battery_array.json (UIDs above PLP max, bus uids resolved) ==="
python3 - <<PYEOF
import json, sys
from pathlib import Path

case_dir = Path("${CASE_DIR}")
plp_json = Path("${PLP_JSON}")
bess_src = Path("${BESS_SRC}")

with open(plp_json, "r", encoding="utf-8") as f:
    planning = json.load(f)
buses_by_name = {b["name"]: b["uid"] for b in planning["system"]["bus_array"]}
existing_uids = []
for arr in planning["system"].values():
    if isinstance(arr, list):
        for el in arr:
            if isinstance(el, dict) and "uid" in el:
                existing_uids.append(el["uid"])
max_uid = max(existing_uids) if existing_uids else 0
print(f"  PLP base: max uid across all arrays = {max_uid}")

with open(bess_src, "r", encoding="utf-8") as f:
    bess = json.load(f)

# Self-discharge: 2 % per month → annual_loss = 12 × 0.02 = 0.24 [p.u./year]
# gtopt's storage_lp.cpp converts this to a per-hour decay factor
# (annual_loss / 8760), so the SoC drops by ~2 % across a 730-hour
# month under no charge/discharge.  Mirrors the industry standard for
# li-ion BESS (~2-3 %/month at 25 °C ambient).
MONTHLY_LOSS_FRAC = 0.02
ANNUAL_LOSS = 12 * MONTHLY_LOSS_FRAC

# Renumber BESS UIDs starting at max_uid + 1; drop eini + add annual_loss.
unresolved = []
for i, b in enumerate(bess["battery_array"], 1):
    b["uid"] = max_uid + i
    bus_name = b.get("bus")
    # gtopt's ``Battery.bus`` is a SingleId = variant<Uid, Name>, so
    # keep the human-readable name (e.g. ``"DonGoyo220"``) verbatim and
    # let gtopt resolve it against ``bus_array`` at LP-build time.
    # Validate that the name actually exists in the PLP base so we
    # don't ship a dangling reference; warn but keep the entry
    # otherwise (some BESS substations are regional proxies).
    if bus_name not in buses_by_name:
        unresolved.append((b["name"], bus_name))
    # Drop ``eini`` — leaving the initial SoC unset lets the LP pick
    # the optimal start-of-horizon level (subject to ``emin``).  The
    # 50 %-of-emax default in the source JSON was an arbitrary guess
    # that biases the daily-cycle optimum.
    b.pop("eini", None)
    # 2 %/month self-discharge.
    b["annual_loss"] = ANNUAL_LOSS
if unresolved:
    print(f"WARNING: unresolved bus names → {unresolved}", file=sys.stderr)

# Write the BESS overlay as a top-level Planning JSON (gtopt merges
# at the system_array level, so wrap the array in {"system": …}).
# Provenance goes to a sidecar — gtopt's Planning schema rejects extra
# top-level keys, so we cannot keep ``_provenance`` inside the file
# that gtopt parses.  See bess_battery_array.provenance.json for the
# source / resolution rationale.
overlay = {"system": {"battery_array": bess["battery_array"]}}
out_path = case_dir / "bess_battery_array.json"
out_path.write_text(json.dumps(overlay, indent=2, ensure_ascii=False))
sidecar = case_dir / "bess_battery_array.provenance.json"
sidecar.write_text(json.dumps({"_provenance": bess["_provenance"]}, indent=2, ensure_ascii=False))
print(f"  written: {out_path}")
print(f"  BESS UIDs: {[b['uid'] for b in bess['battery_array']]}")
print(f"  Total batteries after merge: PLP({len(planning['system']['battery_array'])}) + new({len(bess['battery_array'])})")
PYEOF

echo
echo "=== Build complete ==="
echo "Files in ${CASE_DIR}:"
ls -la "${CASE_DIR}"
echo
echo "Next: run ./run.sh to invoke gtopt on the assembled case."

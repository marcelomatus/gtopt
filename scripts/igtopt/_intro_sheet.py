# SPDX-License-Identifier: BSD-3-Clause
"""Introduction-sheet content + the two static-content sheet builders.

Holds:

* ``_INTRO_LINES``     — every row of the ``.introduction`` worksheet.
* ``_EXAMPLES``        — example data rows for a handful of the system sheets.
* ``_build_introduction_sheet`` and ``_build_boundary_cuts_sheet`` — the
  two builders that consume only static content + a ``_WorkbookStyles``
  object (no ``FIELD_META`` dependency, so they live in their own module).

Imported by :mod:`igtopt.template_builder`.  ``_WorkbookStyles`` is passed
in by the caller so this module has no dependency on openpyxl beyond what
the builders themselves import lazily.
"""

from __future__ import annotations

from typing import Any

# ------------------------------------------------------------------
# Introduction sheet content
# ------------------------------------------------------------------
_INTRO_LINES = [
    ("gtopt Planning Template", "TITLE"),
    ("", ""),
    (
        "This workbook is a ready-to-use template for building gtopt planning "
        "cases with the igtopt converter.",
        "BODY",
    ),
    (
        "Fill in each sheet with your system data, then run:  igtopt <this_file>.xlsx",
        "BODY",
    ),
    ("", ""),
    ("Sheet Guide", "HEADING"),
    ("", ""),
    ("Sheet name", "Header", "Description", "TABLE_HEADER"),
    ("options", "", "Global solver and I/O options (one row per option key)", "TABLE"),
    (
        "block_array",
        "Simulation",
        "Operating time blocks (hours)",
        "TABLE",
    ),
    (
        "stage_array",
        "Simulation",
        "Planning stages (groups of blocks)",
        "TABLE",
    ),
    (
        "scenario_array",
        "Simulation",
        "Probability-weighted scenarios",
        "TABLE",
    ),
    (
        "phase_array",
        "Simulation",
        "SDDP phases (groups of stages) — leave empty for monolithic",
        "TABLE",
    ),
    (
        "scene_array",
        "Simulation",
        "SDDP scenes (groups of scenarios) — leave empty for monolithic",
        "TABLE",
    ),
    (
        "aperture_array",
        "Simulation",
        "SDDP backward-pass apertures (scenario sampling) — leave empty for default",
        "TABLE",
    ),
    (
        "iteration_array",
        "Simulation",
        "SDDP per-iteration control flags — leave empty for defaults",
        "TABLE",
    ),
    ("bus_array", "System", "Electrical busbars (nodes)", "TABLE"),
    (
        "generator_array",
        "System",
        "Thermal / renewable / hydro generators",
        "TABLE",
    ),
    (
        "generator_profile_array",
        "System",
        "Time-varying capacity-factor profiles for generators",
        "TABLE",
    ),
    ("demand_array", "System", "Electricity consumers (loads)", "TABLE"),
    (
        "demand_profile_array",
        "System",
        "Time-varying load-shape profiles for demands",
        "TABLE",
    ),
    ("line_array", "System", "Transmission lines / branches", "TABLE"),
    (
        "battery_array",
        "System",
        "Energy storage devices (batteries, pumped-hydro, etc.)",
        "TABLE",
    ),
    (
        "converter_array",
        "System",
        "Battery charge/discharge converter links",
        "TABLE",
    ),
    (
        "lng_terminal_array",
        "System",
        "LNG storage terminals with generator fuel coupling",
        "TABLE",
    ),
    ("reserve_zone_array", "System", "Spinning-reserve zones", "TABLE"),
    (
        "reserve_provision_array",
        "System",
        "Generator contributions to reserve zones",
        "TABLE",
    ),
    ("junction_array", "System", "Hydraulic junctions (nodes)", "TABLE"),
    (
        "waterway_array",
        "System",
        "Water channels between junctions",
        "TABLE",
    ),
    (
        "flow_array",
        "System",
        "Exogenous inflows / outflows at junctions",
        "TABLE",
    ),
    (
        "reservoir_array",
        "System",
        "Water reservoirs (lakes, dams)",
        "TABLE",
    ),
    (
        "reservoir_seepage_array",
        "System",
        "Water seepage from waterways into reservoirs",
        "TABLE",
    ),
    (
        "reservoir_discharge_limit_array",
        "System",
        "Volume-dependent discharge limits for reservoirs (Ralco-type constraints)",
        "TABLE",
    ),
    (
        "turbine_array",
        "System",
        "Hydro turbines linking waterways to generators",
        "TABLE",
    ),
    (
        "reservoir_production_factor_array",
        "System",
        "Volume-dependent turbine productivity curves",
        "TABLE",
    ),
    (
        "user_constraint_array",
        "System",
        "User-defined linear constraints (AMPL-inspired syntax)",
        "TABLE",
    ),
    ("", ""),
    ("Time-series Sheets (@-sheets)", "HEADING"),
    ("", ""),
    (
        "Sheets named  Element@field  (e.g. 'Demand@lmax', 'GeneratorProfile@profile')",
        "BODY",
    ),
    (
        "are written as Parquet/CSV files to the input directory. Column headers",
        "BODY",
    ),
    (
        "must be: scenario, stage, block, then one column per element (by name).",
        "BODY",
    ),
    ("", ""),
    ("Conventions", "HEADING"),
    ("", ""),
    ("• Sheets / columns whose name starts with '.' are silently skipped.", "BODY"),
    (
        "• Leave cells empty (or use NaN) for optional fields — they are omitted from JSON.",
        "BODY",
    ),
    (
        "• Fields of type  number|array|filename  accept a scalar, an inline array",
        "BODY",
    ),
    ("  (JSON syntax, e.g. [100,90,80]), or a filename stem.", "BODY"),
    (
        "• The 'active' column accepts 1 (active) or 0 (inactive); "
        "leave blank to use the default (1).",
        "BODY",
    ),
    ("", ""),
    (
        "Generated by igtopt --make-template — re-run to refresh after C++ changes.",
        "FOOTER",
    ),
]

# ------------------------------------------------------------------
# Example rows for common sheets
# ------------------------------------------------------------------
_EXAMPLES: dict[str, list[dict[str, Any]]] = {
    "block_array": [{"uid": 1, "name": "b1", "duration": 1.0}],
    "stage_array": [{"uid": 1, "name": "s1", "first_block": 0, "count_block": 8760}],
    "scenario_array": [
        {"uid": 1, "name": "sc1", "active": 1, "probability_factor": 1.0}
    ],
    "bus_array": [
        {"uid": 1, "name": "b1", "voltage": 220.0},
        {"uid": 2, "name": "b2", "voltage": 220.0},
    ],
    "generator_array": [
        {"uid": 1, "name": "g1", "bus": "b1", "pmax": 100.0, "gcost": 20.0},
        {"uid": 2, "name": "g2", "bus": "b2", "pmax": 200.0, "gcost": 35.0},
    ],
    "demand_array": [
        {"uid": 1, "name": "d1", "bus": "b1", "lmax": 80.0},
        {"uid": 2, "name": "d2", "bus": "b2", "lmax": 150.0},
    ],
    "line_array": [
        {
            "uid": 1,
            "name": "l1_2",
            "bus_a": "b1",
            "bus_b": "b2",
            "tmax_ab": 200.0,
            "tmax_ba": 200.0,
            "reactance": 0.05,
        }
    ],
    "options": [],  # handled separately — no example rows in data sheet
}


def _build_introduction_sheet(ws: Any, styles: Any) -> None:
    """Populate the .introduction worksheet."""
    ws.column_dimensions["A"].width = 28
    ws.column_dimensions["B"].width = 14
    ws.column_dimensions["C"].width = 58

    row = 1
    for entry in _INTRO_LINES:
        kind = entry[-1]
        if kind == "TITLE":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = styles.title_font
            ws.merge_cells(f"A{row}:C{row}")
        elif kind == "HEADING":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = styles.heading_font
            ws.merge_cells(f"A{row}:C{row}")
        elif kind == "BODY":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = styles.body_font
            ws.merge_cells(f"A{row}:C{row}")
        elif kind == "FOOTER":
            cell = ws.cell(row=row, column=1, value=entry[0])
            cell.font = styles.footer_font
            ws.merge_cells(f"A{row}:C{row}")
        elif kind == "TABLE_HEADER":
            for col_idx, val in enumerate(entry[:3], start=1):
                c = ws.cell(row=row, column=col_idx, value=val)
                c.font = styles.table_header_font
                c.fill = styles.table_header_fill
        elif kind == "TABLE":
            for col_idx, val in enumerate(entry[:3], start=1):
                c = ws.cell(row=row, column=col_idx, value=val)
                c.font = styles.body_font
                if row % 2 == 0:
                    c.fill = styles.alt_row_fill
        # blank spacer rows (entry[0] == "") are left empty
        row += 1


def _build_boundary_cuts_sheet(wb: Any, styles: Any) -> None:
    """Create the boundary_cuts example worksheet."""
    ws_bc = wb.create_sheet("boundary_cuts")
    bc_headers = [
        "name",
        "iteration",
        "scene",
        "rhs",
        "Reservoir1",
        "Reservoir2",
    ]
    for col_idx, header in enumerate(bc_headers, start=1):
        cell = ws_bc.cell(row=1, column=col_idx, value=header)
        cell.font = styles.header_font_white
        cell.fill = styles.header_fill
    # Example row
    ws_bc.cell(row=2, column=1, value="bc_1_1")
    ws_bc.cell(row=2, column=2, value=1)
    ws_bc.cell(row=2, column=3, value=1)
    ws_bc.cell(row=2, column=4, value=-5000.0)
    ws_bc.cell(row=2, column=5, value=0.25)
    ws_bc.cell(row=2, column=6, value=0.75)
    ws_bc.freeze_panes = "A2"
    # Add a note explaining the sheet
    ws_bc.cell(
        row=4,
        column=1,
        value="# Columns after 'rhs' are state-variable names",
    )
    ws_bc.cell(
        row=5,
        column=1,
        value="# (reservoir/junction names). Values are gradient coefficients.",
    )
    ws_bc.cell(
        row=6,
        column=1,
        value="# 'iteration' = SDDP iteration (PLP IPDNumIte); "
        "'scene' = scene UID (PLP ISimul maps to scene UID in plp2gtopt).",
    )

# SPDX-License-Identifier: BSD-3-Clause
"""Planning structure diagram — pure SVG / Mermaid / HTML generators.

Generates visual representations of the gtopt planning time hierarchy:
scenarios, phases, stages, blocks, and the objective function formula.
"""

from __future__ import annotations

import textwrap

# ---------------------------------------------------------------------------
# Default planning data (used when no JSON file is provided)
# ---------------------------------------------------------------------------

_DEFAULT_PLANNING: dict = {
    "system": {"name": "gtopt (example)"},
    "simulation": {
        "scenario_array": [
            {"uid": 1, "name": "Scenario 1 (dry year)", "probability_factor": 0.4},
            {"uid": 2, "name": "Scenario 2 (normal)", "probability_factor": 0.4},
            {"uid": 3, "name": "Scenario 3 (wet year)", "probability_factor": 0.2},
        ],
        "stage_array": [
            {
                "uid": 1,
                "name": "Stage 1",
                "first_block": 0,
                "count_block": 2,
                "discount_factor": 1.0,
            },
            {
                "uid": 2,
                "name": "Stage 2",
                "first_block": 2,
                "count_block": 2,
                "discount_factor": 0.91,
            },
            {
                "uid": 3,
                "name": "Stage 3",
                "first_block": 4,
                "count_block": 2,
                "discount_factor": 0.83,
            },
        ],
        "phase_array": [
            {"uid": 1, "name": "Phase 1", "first_stage": 0, "count_stage": 2},
            {"uid": 2, "name": "Phase 2", "first_stage": 2, "count_stage": 1},
        ],
        "block_array": [
            {"uid": 1, "name": "Q1", "duration": 2190},
            {"uid": 2, "name": "Q2", "duration": 2190},
            {"uid": 3, "name": "Q3", "duration": 2190},
            {"uid": 4, "name": "Q4", "duration": 2190},
            {"uid": 5, "name": "Y2 H1", "duration": 4380},
            {"uid": 6, "name": "Y2 H2", "duration": 4380},
        ],
    },
}


def _build_planning_svg(planning: dict) -> str:
    """Return a self-contained SVG showing the gtopt planning time structure."""
    if not planning:
        planning = _DEFAULT_PLANNING
    sim = planning.get("simulation", _DEFAULT_PLANNING["simulation"])
    blocks = sim.get("block_array", _DEFAULT_PLANNING["simulation"]["block_array"])
    stages = sim.get("stage_array", _DEFAULT_PLANNING["simulation"]["stage_array"])
    phases = sim.get("phase_array", [])
    scenarios = sim.get(
        "scenario_array", _DEFAULT_PLANNING["simulation"]["scenario_array"]
    )
    sys_name = planning.get("system", {}).get("name", "gtopt")

    n_blocks = len(blocks)
    n_stages = len(stages)
    n_phases = len(phases)
    n_scen = len(scenarios)

    # Layout constants
    M = 48  # left/right margin
    LABEL_W = 120  # width of left label column
    W = max(920, LABEL_W + M + n_blocks * 110)
    TITLE_H = 52
    SEC_GAP = 10
    SCEN_H = 28 + n_scen * 32
    PHASE_H = 48 if n_phases else 0
    STAGE_H = 72
    BLOCK_H = 74
    FORMULA_H = 92
    H = (
        TITLE_H
        + SEC_GAP
        + SCEN_H
        + SEC_GAP
        + PHASE_H
        + STAGE_H
        + BLOCK_H
        + FORMULA_H
        + M
    )

    CONTENT_W = W - LABEL_W - M
    BLOCK_W = CONTENT_W / max(n_blocks, 1)

    def bx(i: int) -> float:  # left edge of block i
        return LABEL_W + i * BLOCK_W

    # Colours
    C_BG = "#FAFBFC"
    C_BORDER = "#CBD5E0"
    C_TITLE = "#1A252F"
    C_SCEN_FILL = "#E8F5E9"
    C_SCEN_BDR = "#2E7D32"
    C_SCEN_TXT = "#1B5E20"
    C_PHASE_FILL = "#E3F2FD"
    C_PHASE_BDR = "#1565C0"
    C_PHASE_TXT = "#0D47A1"
    C_STAGE_FILL = "#FFF8E1"
    C_STAGE_BDR = "#E65100"
    C_STAGE_TXT = "#BF360C"
    C_BLOCK_FILL = "#F3E5F5"
    C_BLOCK_BDR = "#6A1B9A"
    C_BLOCK_TXT = "#4A148C"
    C_FORM_FILL = "#ECEFF1"
    C_FORM_BDR = "#546E7A"
    C_FORM_TXT = "#37474F"
    C_LINESEP = "#CFD8DC"
    F = "font-family='Arial, sans-serif'"

    def r(x, y, w, h, fill, stroke, rx=6, sw=1.5):
        return (
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
            f'rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'
        )

    def t(
        x,
        y,
        s,
        size=12,
        color="#1A252F",
        anchor="middle",
        weight="normal",
        italic=False,
    ):
        style = " font-style='italic'" if italic else ""
        return (
            f'<text x="{x:.1f}" y="{y:.1f}" {F} font-size="{size}" fill="{color}" '
            f'text-anchor="{anchor}" font-weight="{weight}"{style}>{s}</text>'
        )

    def ln(x1, y1, x2, y2, color=C_LINESEP, sw=1, dash=""):
        da = f' stroke-dasharray="{dash}"' if dash else ""
        return (
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{color}" stroke-width="{sw}"{da}/>'
        )

    el: list[str] = []

    # Canvas
    el.append(f'<rect width="{W}" height="{H}" fill="{C_BG}"/>')
    el.append(r(3, 3, W - 6, H - 6, "none", C_BORDER, rx=10, sw=2))

    # Title
    el.append(
        t(
            W / 2,
            32,
            f"{sys_name} \u2014 Planning Time Structure",
            size=17,
            weight="bold",
            color=C_TITLE,
        )
    )
    el.append(ln(M, 46, W - M, 46, color=C_LINESEP, sw=1.5))

    y = TITLE_H + SEC_GAP

    # ---- SCENARIOS ---------------------------------------------------------
    lx = LABEL_W - 8
    el.append(
        t(
            lx,
            y + 16,
            "SCENARIOS",
            size=11,
            weight="bold",
            color=C_SCEN_BDR,
            anchor="end",
        )
    )
    el.append(
        t(
            lx,
            y + 30,
            "(parallel futures,",
            size=8,
            color=C_SCEN_BDR,
            anchor="end",
            italic=True,
        )
    )
    el.append(
        t(
            lx,
            y + 41,
            "weighted by prob)",
            size=8,
            color=C_SCEN_BDR,
            anchor="end",
            italic=True,
        )
    )
    bw = W - LABEL_W - M
    for si, sc in enumerate(scenarios[:8]):
        sy = y + 8 + si * 32
        prob = sc.get("probability_factor", "")
        sc_name = sc.get("name") or f"Scenario {sc.get('uid', si + 1)}"
        el.append(r(LABEL_W, sy, bw, 26, C_SCEN_FILL, C_SCEN_BDR, rx=5, sw=1.5))
        lbl = f"{sc_name}  \u2014  probability_factor = {prob}"
        el.append(t(LABEL_W + bw / 2, sy + 17, lbl, size=11, color=C_SCEN_TXT))
    if n_scen > 8:
        sy = y + 8 + 8 * 32
        el.append(
            t(
                LABEL_W + bw / 2,
                sy + 10,
                f"\u2026 and {n_scen - 8} more scenarios",
                size=10,
                color=C_SCEN_BDR,
                italic=True,
            )
        )
    y += SCEN_H + SEC_GAP

    # ---- PHASES (optional) -------------------------------------------------
    if n_phases:
        el.append(
            t(
                lx,
                y + 18,
                "PHASES",
                size=11,
                weight="bold",
                color=C_PHASE_BDR,
                anchor="end",
            )
        )
        el.append(
            t(
                lx,
                y + 30,
                "(group stages)",
                size=8,
                color=C_PHASE_BDR,
                anchor="end",
                italic=True,
            )
        )
        for ph in phases:
            fs = ph.get("first_stage", 0)
            cs = ph.get("count_stage", n_stages)
            if fs >= n_stages:
                continue
            last_s = min(fs + cs - 1, n_stages - 1)
            st0 = stages[fs]
            stL = stages[last_s]
            fb0 = st0.get("first_block", 0)
            fbL = stL.get("first_block", last_s)
            cbL = stL.get("count_block", 1)
            px = bx(fb0) + 1
            pw = bx(fbL + cbL) - px - 1
            ph_name = ph.get("name") or f"Phase {ph.get('uid', '')}"
            el.append(
                r(px, y + 4, pw, PHASE_H - 10, C_PHASE_FILL, C_PHASE_BDR, rx=5, sw=1.5)
            )
            el.append(
                t(
                    px + pw / 2,
                    y + 24,
                    ph_name,
                    size=11,
                    color=C_PHASE_TXT,
                    weight="bold",
                )
            )
        y += PHASE_H + SEC_GAP

    # ---- STAGES ------------------------------------------------------------
    el.append(
        t(lx, y + 22, "STAGES", size=11, weight="bold", color=C_STAGE_BDR, anchor="end")
    )
    el.append(
        t(
            lx,
            y + 34,
            "(investment period,",
            size=8,
            color=C_STAGE_BDR,
            anchor="end",
            italic=True,
        )
    )
    el.append(
        t(
            lx,
            y + 45,
            "discounted cost)",
            size=8,
            color=C_STAGE_BDR,
            anchor="end",
            italic=True,
        )
    )
    for st in stages:
        fb = st.get("first_block", 0)
        cb = st.get("count_block", 1)
        if fb >= n_blocks:
            continue
        sx = bx(fb) + 1
        sw2 = bx(min(fb + cb, n_blocks)) - sx - 1
        df = st.get("discount_factor", "")
        st_name = st.get("name") or f"Stage {st.get('uid', '')}"
        el.append(
            r(sx, y + 4, sw2, STAGE_H - 10, C_STAGE_FILL, C_STAGE_BDR, rx=5, sw=1.5)
        )
        el.append(
            t(sx + sw2 / 2, y + 26, st_name, size=10, color=C_STAGE_TXT, weight="bold")
        )
        info = f"discount={df}" if df != "" else f"{cb} block(s)"
        el.append(
            t(sx + sw2 / 2, y + 42, info, size=9, color=C_STAGE_BDR, italic=df == "")
        )
    y += STAGE_H

    # ---- BLOCKS ------------------------------------------------------------
    el.append(
        t(lx, y + 20, "BLOCKS", size=11, weight="bold", color=C_BLOCK_BDR, anchor="end")
    )
    el.append(
        t(
            lx,
            y + 33,
            "(time steps,",
            size=8,
            color=C_BLOCK_BDR,
            anchor="end",
            italic=True,
        )
    )
    el.append(
        t(
            lx,
            y + 44,
            "energy=power\u00d7dur.)",
            size=8,
            color=C_BLOCK_BDR,
            anchor="end",
            italic=True,
        )
    )
    for bi, blk in enumerate(blocks):
        bxi = bx(bi) + 1
        bwi = BLOCK_W - 2
        dur = blk.get("duration", "")
        b_name = blk.get("name") or f"B{blk.get('uid', bi + 1)}"
        el.append(
            r(bxi, y + 4, bwi, BLOCK_H - 10, C_BLOCK_FILL, C_BLOCK_BDR, rx=4, sw=1.5)
        )
        fsize = max(8, min(11, int(bwi / 7)))
        el.append(
            t(
                bxi + bwi / 2,
                y + 26,
                b_name,
                size=fsize,
                color=C_BLOCK_TXT,
                weight="bold",
            )
        )
        if dur != "":
            d_str = f"{dur}h" if float(dur) < 1000 else f"{float(dur) / 8760:.2f}yr"
            el.append(
                t(
                    bxi + bwi / 2,
                    y + 44,
                    d_str,
                    size=max(7, fsize - 1),
                    color=C_BLOCK_BDR,
                )
            )
    y += BLOCK_H

    # ---- OBJECTIVE FUNCTION ------------------------------------------------
    el.append(
        r(
            LABEL_W,
            y + 8,
            W - LABEL_W - M,
            FORMULA_H - 16,
            C_FORM_FILL,
            C_FORM_BDR,
            rx=8,
            sw=1.5,
        )
    )
    fy = y + 28
    el.append(
        t(
            LABEL_W + 14,
            fy,
            "OBJECTIVE FUNCTION:",
            size=11,
            weight="bold",
            color=C_TITLE,
            anchor="start",
        )
    )
    fy += 20
    el.append(
        t(
            LABEL_W + 18,
            fy,
            "min  \u03a3_s  \u03a3_t  \u03a3_b   prob_s \u00d7 discount_t"
            " \u00d7 duration_b \u00d7 dispatch_cost(s, t, b)",
            size=12,
            color=C_FORM_TXT,
            anchor="start",
        )
    )
    fy += 18
    el.append(
        t(
            LABEL_W + 18,
            fy,
            "+  \u03a3_t   annual_capcost_t \u00d7 expansion_modules_t      [CAPEX / investment]",
            size=12,
            color=C_FORM_TXT,
            anchor="start",
        )
    )
    fy += 16
    el.append(
        t(
            LABEL_W + 18,
            fy,
            "subject to: bus power balance, Kirchhoff voltage law,"
            " generator / line / battery / hydro constraints",
            size=10,
            color="#78909C",
            anchor="start",
            italic=True,
        )
    )

    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
        f'width="{W}" height="{H}">\n' + "\n".join(el) + "\n</svg>"
    )


def _build_planning_mermaid(_planning: dict) -> str:  # noqa: ARG001
    """Return a Mermaid classDiagram of the gtopt planning data model.

    The *_planning* argument is accepted for API consistency but is not used
    because the class diagram describes the generic data model rather than
    instance-specific data.
    """
    lines = [
        "```mermaid",
        "---",
        "title: gtopt Planning Data Model",
        "---",
        "classDiagram",
        "    direction TB",
        "",
        "    class Planning {",
        "        +string name",
        "        +PlanningOptions options",
        "    }",
        "    class Simulation {",
        "        +Scenario[] scenario_array",
        "        +Stage[]    stage_array",
        "        +Phase[]    phase_array",
        "        +Block[]    block_array",
        "    }",
        "    class Scenario {",
        "        +Uid  uid",
        "        +Name name",
        "        +Real probability_factor [p.u.]",
        "        +Bool active",
        "    }",
        "    class Phase {",
        "        +Uid  uid",
        "        +Name name",
        "        +Size first_stage",
        "        +Size count_stage",
        "    }",
        "    class Stage {",
        "        +Uid  uid",
        "        +Name name",
        "        +Size first_block",
        "        +Size count_block",
        "        +Real discount_factor [p.u.]",
        "        +Bool active",
        "    }",
        "    class Block {",
        "        +Uid  uid",
        "        +Name name",
        "        +Real duration [h]",
        "    }",
        "    class Scene {",
        "        +Scenario scenario",
        "        +Phase    phase",
        "        <<internal>>",
        "    }",
        "    class System {",
        "        +Bus[]       bus_array",
        "        +Generator[] generator_array",
        "        +Demand[]    demand_array",
        "        +Line[]      line_array",
        "        +Battery[]   battery_array",
        "        +Converter[] converter_array",
        "        +Junction[]  junction_array",
        "        +Reservoir[] reservoir_array",
        "        +Turbine[]   turbine_array",
        "    }",
        "",
        "    Planning  *--  Simulation : contains",
        "    Planning  *--  System     : contains",
        "    Simulation *-- Scenario   : has many",
        "    Simulation *-- Stage      : has many",
        "    Simulation *-- Phase      : has many optional",
        "    Simulation *-- Block      : has many",
        "    Phase      o-- Stage      : groups",
        "    Stage      o-- Block      : references by first_block+count_block",
        "    Scene      --> Scenario   : cross-product",
        "    Scene      --> Phase      : cross-product",
        "```",
    ]
    return "\n".join(lines)


def _build_planning_html(
    planning: dict, title: str = "gtopt Planning Structure"
) -> str:
    """Self-contained HTML with the planning SVG and Mermaid class diagram."""
    svg_content = _build_planning_svg(planning)
    mmd_inner = "\n".join(_build_planning_mermaid(planning).splitlines()[4:-1])

    return textwrap.dedent(f"""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
<style>
  body {{ font-family: Arial, sans-serif; background: #F4F6F7; margin: 0; padding: 20px; }}
  h1   {{ color: #1A252F; font-size: 22px; margin-bottom: 4px; }}
  h2   {{ color: #2C3E50; font-size: 16px; margin: 28px 0 8px 0;
           border-bottom: 2px solid #AED6F1; padding-bottom: 4px; }}
  .card {{ background: white; border: 1px solid #D5D8DC; border-radius: 10px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.07); padding: 16px;
            margin-bottom: 22px; overflow-x: auto; }}
  .card svg {{ max-width: 100%; height: auto; display: block; }}
  .mermaid   {{ text-align: center; }}
  .note  {{ color: #7F8C8D; font-size: 12px; margin-top: 4px; }}
</style>
</head>
<body>
<h1>\U0001f5d3\ufe0f gtopt — Planning Time Structure</h1>
<p class="note">
  Multi-stage planning time hierarchy: Scenarios (uncertainty) &rarr;
  Phases (investment groups, optional) &rarr; Stages (investment periods) &rarr;
  Blocks (operational time steps).
</p>

<h2>\u23f1\ufe0f Time Structure Diagram</h2>
<div class="card">{svg_content}</div>

<h2>\U0001f5c2\ufe0f Data Model (UML Class Diagram)</h2>
<div class="card">
  <div class="mermaid">
{mmd_inner}
  </div>
</div>

<script>
  mermaid.initialize({{ startOnLoad: true, theme: 'default',
    themeVariables: {{ fontSize: '13px' }} }});
</script>
</body>
</html>
""")

#!/usr/bin/env python3
"""
gtopt_diagram.py — Generate electrical, hydro and planning-structure diagrams
from gtopt JSON planning files.

This module is installed as the ``gtopt_diagram`` command-line tool.
See ``DIAGRAM_TOOL.md`` for the full user guide.

Diagram types (--diagram-type)
-------------------------------
``topology``  — Network topology diagram (default).
``planning``  — Planning time hierarchy: scenarios → phases → stages → blocks.

Subsystems for topology diagrams (--subsystem)
-----------------------------------------------
``electrical`` — Buses, generators, demands, lines, batteries, converters.
``hydro``      — Junctions, waterways, reservoirs, turbines, flows, filtrations.
``full``       — Both subsystems together (default).

Output formats (--format)
--------------------------
``svg``      — Scalable SVG via Graphviz (recommended for docs).
``png``      — Rasterised PNG via Graphviz.
``pdf``      — PDF via Graphviz.
``dot``      — Raw Graphviz DOT source.
``mermaid``  — Mermaid flowchart source (embeds in GitHub Markdown).
``html``     — Interactive HTML with vis.js / font-awesome (open in browser).

Aggregation / reduction (--aggregate, default: auto)
-----------------------------------------------------
``auto``     Smart selection based on total element count (default):
               < 100  elements → ``none``  (show every element individually)
               100-999 elements → ``bus``  (one summary node per bus)
               ≥ 1000 elements  → ``type`` + smart voltage threshold
                                  (aggregate by type; threshold chosen so ≤ 64 buses remain)
``none``     Show every individual element.
``bus``      One summary node per bus.
``type``     One node per (bus, generator-type) pair with icon.
             Types: 💧 hydro ☀️ solar 🌬️ wind ⚡ thermal 🔋 battery.
``global``   One node per generator type for the whole system.

``--no-generators``  Omit all generator nodes (topology-only view):
                     buses, lines, demands, hydro elements only.

Other reduction options (--voltage-threshold, --filter-type, …)
----------------------------------------------------------------
``--voltage-threshold KV``   Lump buses below KV into their nearest HV neighbour.
``--filter-type TYPE...``    Show only generators of listed types.
``--focus-bus BUS...``       Show only N-hop neighbourhood of named buses.
``--top-gens N``             Keep only top-N generators per bus by pmax.
``--max-nodes N``            Escalate aggregation mode until ≤ N nodes.
``--compact``                Omit pmax/gcost/reactance labels.
``--hide-isolated``          Remove nodes with no connections.

Dependencies
------------
    pip install "gtopt-scripts[diagram]"   # installs graphviz pyvis cairosvg
    sudo apt-get install graphviz

Usage examples
--------------
    # Auto mode (default) — picks right strategy automatically
    gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg       # <100 → none
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o c2y.svg  # ≥1000 → type+smart threshold

    # Topology-only (no generators) — clean network view
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --no-generators -o topo.svg

    # Force explicit aggregation
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate type --voltage-threshold 220 --compact -o case2y_hv.svg

    # Global summary (one node per generator type)
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate global --compact -o case2y_global.svg

    # Hydro cascade only
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --subsystem hydro -o case2y_hydro.svg

    # Interactive HTML for exploration
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate type --voltage-threshold 100 --compact \\
        --format html -o case2y.html

    # Planning structure
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --diagram-type planning -o case2y_planning.svg

    # Mermaid snippet for GitHub Markdown
    gtopt_diagram cases/ieee_9b/ieee_9b.json --format mermaid
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import logging
import os
import sys
import tempfile
import textwrap
import webbrowser
from collections import defaultdict
from pathlib import Path
from typing import Optional

from gtopt_diagram._classify import (
    classify_gen as _classify_gen_impl,
    dominant_kind as _dominant_kind_impl,
    efficiency_turbine_pairs as _efficiency_turbine_pairs_impl,
    elem_name as _elem_name_impl,
    gen_pmax as _gen_pmax_impl,
    icon_key_for_type as _icon_key_for_type_impl,
    resolve as _resolve_impl,
    scalar as _scalar_impl,
    turbine_gen_refs as _turbine_gen_refs_impl,
    turbine_waterway_refs as _turbine_waterway_refs_impl,
)
from gtopt_diagram._classify import GEN_TYPE_META as _GEN_TYPE_META
from gtopt_diagram._data_utils import (
    auto_voltage_threshold,
    build_voltage_map as _build_voltage_map_impl,
    count_visible_buses as _count_visible_buses_impl,
    resolve_bus_ref as _resolve_bus_ref_impl,
)
from gtopt_diagram._graph_model import (
    AUTO_BUS_THRESHOLD as _AUTO_BUS_THRESHOLD,
    AUTO_MAX_HV_BUSES as _AUTO_MAX_HV_BUSES,
    AUTO_NONE_THRESHOLD as _AUTO_NONE_THRESHOLD,
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# SVG icon definitions — power systems standard iconography  48x48px
# ---------------------------------------------------------------------------

_ICON_SVG: dict[str, str] = {}

_ICON_SVG["bus"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#AED6F1"/><stop offset="100%" stop-color="#2E86C1"/>
  </linearGradient></defs>
  <rect x="3" y="19" width="42" height="10" rx="2" fill="url(#g)" stroke="#1A5276" stroke-width="2.5"/>
  <line x1="12" y1="4"  x2="12" y2="19" stroke="#1A5276" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="24" y1="4"  x2="24" y2="19" stroke="#1A5276" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="36" y1="4"  x2="36" y2="19" stroke="#1A5276" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="12" y1="29" x2="12" y2="44" stroke="#1A5276" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="36" y1="29" x2="36" y2="44" stroke="#1A5276" stroke-width="2.5" stroke-linecap="round"/>
  <circle cx="12" cy="4"  r="3" fill="#1A5276"/><circle cx="24" cy="4"  r="3" fill="#1A5276"/>
  <circle cx="36" cy="4"  r="3" fill="#1A5276"/><circle cx="12" cy="44" r="3" fill="#1A5276"/>
  <circle cx="36" cy="44" r="3" fill="#1A5276"/>
</svg>"""

_ICON_SVG["gen"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#FEF9E7"/><stop offset="100%" stop-color="#F0B27A"/>
  </linearGradient></defs>
  <circle cx="24" cy="26" r="18" fill="url(#g)" stroke="#E67E22" stroke-width="2.5"/>
  <path d="M9 26 C12 20 15 20 18 26 C21 32 24 32 27 26 C30 20 33 20 36 26 C37 29 38 29 39 26"
        fill="none" stroke="#E67E22" stroke-width="2" stroke-linecap="round"/>
  <line x1="24" y1="4" x2="24" y2="9" stroke="#E67E22" stroke-width="2.5" stroke-linecap="round"/>
  <circle cx="24" cy="4" r="3" fill="#E67E22"/>
</svg>"""

_ICON_SVG["gen_solar"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#FDEBD0"/><stop offset="100%" stop-color="#F8C471"/>
  </linearGradient></defs>
  <circle cx="24" cy="24" r="20" fill="url(#g)" stroke="#F39C12" stroke-width="2.5"/>
  <circle cx="24" cy="24" r="7"  fill="#F39C12"/>
  <line x1="24" y1="6"  x2="24" y2="14" stroke="#F39C12" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="24" y1="34" x2="24" y2="42" stroke="#F39C12" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="6"  y1="24" x2="14" y2="24" stroke="#F39C12" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="34" y1="24" x2="42" y2="24" stroke="#F39C12" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="11" y1="11" x2="17" y2="17" stroke="#F39C12" stroke-width="2" stroke-linecap="round"/>
  <line x1="31" y1="31" x2="37" y2="37" stroke="#F39C12" stroke-width="2" stroke-linecap="round"/>
  <line x1="37" y1="11" x2="31" y2="17" stroke="#F39C12" stroke-width="2" stroke-linecap="round"/>
  <line x1="11" y1="37" x2="17" y2="31" stroke="#F39C12" stroke-width="2" stroke-linecap="round"/>
</svg>"""

_ICON_SVG["gen_hydro"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#D1F2EB"/><stop offset="100%" stop-color="#2ECC71"/>
  </linearGradient></defs>
  <circle cx="24" cy="24" r="20" fill="url(#g)" stroke="#1E8449" stroke-width="2.5"/>
  <text x="24" y="21" text-anchor="middle" font-family="Arial" font-weight="bold"
        font-size="13" fill="#145A32">G</text>
  <path d="M24 25 Q19 30 19 34 Q19 39 24 39 Q29 39 29 34 Q29 30 24 25"
        fill="#2980B9" stroke="#1A5276" stroke-width="1.2"/>
  <line x1="24" y1="4" x2="24" y2="9" stroke="#1E8449" stroke-width="2.5" stroke-linecap="round"/>
  <circle cx="24" cy="4" r="3" fill="#1E8449"/>
</svg>"""

_ICON_SVG["demand"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#FADBD8"/><stop offset="100%" stop-color="#E74C3C"/>
  </linearGradient></defs>
  <rect x="6" y="14" width="36" height="26" rx="3" fill="url(#g)" stroke="#C0392B" stroke-width="2.5"/>
  <polyline points="11,22 16,17 21,27 26,17 31,27 36,22"
            fill="none" stroke="#C0392B" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
  <line x1="24" y1="6"  x2="24" y2="14" stroke="#C0392B" stroke-width="2.5" stroke-linecap="round"/>
  <circle cx="24" cy="6" r="3" fill="#C0392B"/>
  <line x1="24" y1="40" x2="24" y2="44" stroke="#C0392B" stroke-width="2.5" stroke-linecap="round"/>
  <polygon points="24,47 20,41 28,41" fill="#C0392B"/>
</svg>"""

_ICON_SVG["battery"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#E8DAEF"/><stop offset="100%" stop-color="#9B59B6"/>
  </linearGradient></defs>
  <rect x="5"  y="12" width="38" height="26" rx="3" fill="url(#g)" stroke="#7D3C98" stroke-width="2.5"/>
  <rect x="19" y="7"  width="10" height="5"  rx="1" fill="#7D3C98"/>
  <line x1="13" y1="17" x2="13" y2="33" stroke="#7D3C98" stroke-width="3.5" stroke-linecap="round"/>
  <line x1="21" y1="17" x2="21" y2="33" stroke="#7D3C98" stroke-width="3.5" stroke-linecap="round"/>
  <line x1="29" y1="17" x2="29" y2="33" stroke="#7D3C98" stroke-width="3.5" stroke-linecap="round"/>
  <line x1="37" y1="17" x2="37" y2="33" stroke="#7D3C98" stroke-width="3.5" stroke-linecap="round"/>
  <line x1="17" y1="19" x2="17" y2="31" stroke="#D7BDE2" stroke-width="2" stroke-linecap="round"/>
  <line x1="25" y1="19" x2="25" y2="31" stroke="#D7BDE2" stroke-width="2" stroke-linecap="round"/>
  <line x1="33" y1="19" x2="33" y2="31" stroke="#D7BDE2" stroke-width="2" stroke-linecap="round"/>
  <text x="7"  y="44" font-family="Arial" font-weight="bold" font-size="12" fill="#7D3C98">&#8722;</text>
  <text x="38" y="44" font-family="Arial" font-weight="bold" font-size="12" fill="#7D3C98">+</text>
</svg>"""

_ICON_SVG["converter"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="0%">
    <stop offset="0%" stop-color="#D6EAF8"/><stop offset="100%" stop-color="#5DADE2"/>
  </linearGradient></defs>
  <rect x="3" y="8" width="42" height="32" rx="5" fill="url(#g)" stroke="#1F618D" stroke-width="2.5"/>
  <line x1="8"  y1="22" x2="22" y2="22" stroke="#1F618D" stroke-width="2.5"/>
  <polygon points="22,22 17,18 17,26" fill="#1F618D"/>
  <line x1="26" y1="26" x2="40" y2="26" stroke="#1A5276" stroke-width="2.5"/>
  <polygon points="26,26 31,22 31,30" fill="#1A5276"/>
  <text x="5"  y="16" font-family="Arial" font-size="8" font-weight="bold" fill="#1F618D">AC</text>
  <text x="34" y="16" font-family="Arial" font-size="8" font-weight="bold" fill="#1A5276">DC</text>
</svg>"""

_ICON_SVG["junction"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#A9DFBF"/><stop offset="100%" stop-color="#27AE60"/>
  </linearGradient></defs>
  <circle cx="24" cy="24" r="17" fill="url(#g)" stroke="#1E8449" stroke-width="2.5"/>
  <line x1="4"  y1="8"  x2="16" y2="18" stroke="#1E8449" stroke-width="2"/>
  <polygon points="16,18 9,16 13,10" fill="#1E8449"/>
  <line x1="44" y1="24" x2="30" y2="24" stroke="#145A32" stroke-width="2"/>
  <polygon points="44,24 37,20 37,28" fill="#145A32"/>
  <line x1="24" y1="44" x2="24" y2="30" stroke="#145A32" stroke-width="2"/>
  <polygon points="24,44 20,37 28,37" fill="#145A32"/>
</svg>"""

_ICON_SVG["reservoir"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="gw" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#AED6F1"/><stop offset="100%" stop-color="#2980B9"/>
  </linearGradient></defs>
  <polygon points="3,45 11,9 37,9 45,45" fill="#BDC3C7" stroke="#7F8C8D" stroke-width="2.5"/>
  <path d="M13 13 L35 13 L41 45 L7 45 Z" fill="url(#gw)" opacity="0.9"/>
  <path d="M11 27 Q16 23 21 27 Q26 31 31 27 Q36 23 37 27"
        fill="none" stroke="white" stroke-width="1.5" opacity="0.8"/>
  <polygon points="3,45 11,9 37,9 45,45" fill="none" stroke="#7F8C8D" stroke-width="2.5"/>
  <rect x="21" y="38" width="6" height="9" fill="#2980B9" stroke="#1A5276" stroke-width="1"/>
</svg>"""

_ICON_SVG["turbine"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#D1F2EB"/><stop offset="100%" stop-color="#27AE60"/>
  </linearGradient></defs>
  <circle cx="24" cy="24" r="20" fill="url(#g)" stroke="#1E8449" stroke-width="2.5"/>
  <path d="M24 20 Q31 15 33 9  Q26 11 24 20" fill="#27AE60" stroke="#1E8449" stroke-width="1.2"/>
  <path d="M28 24 Q35 21 41 26 Q37 31 28 24" fill="#27AE60" stroke="#1E8449" stroke-width="1.2"/>
  <path d="M24 28 Q19 35 17 41 Q24 39 24 28" fill="#27AE60" stroke="#1E8449" stroke-width="1.2"/>
  <path d="M20 24 Q13 27 7  22 Q11 17 20 24" fill="#27AE60" stroke="#1E8449" stroke-width="1.2"/>
  <circle cx="24" cy="24" r="5" fill="#1E8449"/>
  <circle cx="24" cy="24" r="2" fill="white"/>
</svg>"""

_ICON_SVG["flow"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="0%">
    <stop offset="0%" stop-color="#AED6F1"/><stop offset="100%" stop-color="#3498DB"/>
  </linearGradient></defs>
  <path d="M3 19 Q12 13 21 19 Q30 25 39 19 L43 22 L39 28 Q30 22 21 28 Q12 34 3 28 Z"
        fill="url(#g)" stroke="#2980B9" stroke-width="1.5"/>
  <path d="M6 23 Q11 20 16 23 Q21 26 26 23 Q31 20 36 23"
        fill="none" stroke="white" stroke-width="1.5" opacity="0.7"/>
  <polygon points="46,24 38,19 38,29" fill="#1A5276"/>
</svg>"""

_ICON_SVG[
    "filtration"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#E8E8E8"/><stop offset="100%" stop-color="#95A5A6"/>
  </linearGradient></defs>
  <path d="M3 7 L45 7 L29 27 L29 43 L19 43 L19 27 Z"
        fill="url(#g)" stroke="#717D7E" stroke-width="2.5" stroke-linejoin="round"/>
  <line x1="11" y1="14" x2="37" y2="14" stroke="#7F8C8D" stroke-width="1.5" stroke-dasharray="3,2"/>
  <line x1="16" y1="21" x2="32" y2="21" stroke="#7F8C8D" stroke-width="1.5" stroke-dasharray="3,2"/>
  <ellipse cx="24" cy="47" rx="2.5" ry="3.5" fill="#3498DB" opacity="0.85"/>
</svg>"""

# ---------------------------------------------------------------------------
# Icon utilities — cache SVG → PNG via cairosvg
# ---------------------------------------------------------------------------

_ICON_CACHE: dict[str, str] = {}

# Cache-key suffix — bump when the icon SVG format changes to force re-render
_ICON_CACHE_VER = "v2"

# Mapping from palette keys to generator-type names (used in icon lookups)
_PALETTE_TO_GEN_TYPE: dict[str, str] = {
    "gen": "thermal",
    "gen_solar": "solar",
    "gen_hydro": "hydro",
}

# Layout algorithm thresholds (node count) — chosen empirically
_LAYOUT_DOT_THRESHOLD = 10  # dot:   hierarchical, best for ≤10 nodes
_LAYOUT_NEATO_THRESHOLD = 40  # neato: spring-model, fast for ≤40 nodes
_LAYOUT_FDP_THRESHOLD = 150  # fdp:   force-directed, ≤150 nodes

# Maximum BFS depth — imported from _data_utils for backward compatibility


def _icon_cache_dir() -> str:
    d = os.path.join(tempfile.gettempdir(), "gtopt_icons")
    os.makedirs(d, exist_ok=True)
    return d


def _icon_png_path(kind: str) -> Optional[str]:
    """Return absolute path to a cached PNG icon, creating it if needed.

    Looks up *kind* first in the element-type icon dict (``_ICON_SVG``), then
    falls back to the generator-type specific icons (``_GEN_TYPE_ICON_SVG``).
    Returns *None* when *cairosvg* is not installed.
    """
    # Determine the SVG source — prefer element icons, fall back to gen-type icons
    svg_src = _ICON_SVG.get(kind)
    if svg_src is None:
        gt = _PALETTE_TO_GEN_TYPE.get(kind)
        if gt:
            svg_src = _GEN_TYPE_ICON_SVG.get(gt)
    if svg_src is None:
        return None
    cache_key = f"{kind}_{_ICON_CACHE_VER}"
    if cache_key in _ICON_CACHE:
        return _ICON_CACHE[cache_key]
    try:
        import cairosvg  # noqa: PLC0415
    except ImportError:
        return None
    cache_dir = _icon_cache_dir()
    sig = hashlib.md5(svg_src.encode()).hexdigest()[:8]
    png_path = os.path.join(cache_dir, f"icon_{kind}_{sig}.png")
    if not os.path.exists(png_path):
        png_data = cairosvg.svg2png(
            bytestring=svg_src.encode(), output_width=48, output_height=48
        )
        with open(png_path, "wb") as fh:
            fh.write(png_data)
    _ICON_CACHE[cache_key] = png_path
    return png_path


def _icon_b64_uri(kind: str) -> str:
    """Return a base64 data URI for the best SVG icon for *kind*."""
    svg = _ICON_SVG.get(kind)
    if svg is None:
        gt = _PALETTE_TO_GEN_TYPE.get(kind)
        svg = _GEN_TYPE_ICON_SVG.get(gt, "") if gt else ""
    return (
        ("data:image/svg+xml;base64," + base64.b64encode(svg.encode()).decode())
        if svg
        else ""
    )


# ---------------------------------------------------------------------------
# Colour palette
# ---------------------------------------------------------------------------

_PALETTE: dict[str, str] = {
    "bus": "#D6EAF8",
    "bus_border": "#1A5276",
    "gen": "#FEF9E7",
    "gen_border": "#E67E22",
    "gen_solar": "#FDEBD0",
    "gen_solar_border": "#F39C12",
    "gen_hydro": "#D1F2EB",
    "gen_hydro_border": "#1E8449",
    "demand": "#FADBD8",
    "demand_border": "#C0392B",
    "battery": "#F4ECF7",
    "battery_border": "#7D3C98",
    "converter": "#D6EAF8",
    "converter_border": "#1F618D",
    "junction": "#EAFAF1",
    "junction_border": "#1E8449",
    "reservoir": "#EBF5FB",
    "reservoir_border": "#1A5276",
    "turbine": "#D1F2EB",
    "turbine_border": "#1E8449",
    "flow": "#D6EAF8",
    "flow_border": "#2980B9",
    "filtration": "#EAECEE",
    "filtration_border": "#717D7E",
    "line_edge": "#2C3E50",
    "waterway_edge": "#2980B9",
    "bat_link_edge": "#7D3C98",
    "efficiency_edge": "#A04000",
}

# Colorblind-safe palette (Wong 2011), selected via --palette colorblind
_PALETTE_COLORBLIND: dict[str, str] = {
    "bus": "#E1F5FE",
    "bus_border": "#01579B",
    "gen": "#FFF3E0",
    "gen_border": "#E65100",
    "gen_solar": "#FFFDE7",
    "gen_solar_border": "#F57F17",
    "gen_hydro": "#E8F5E9",
    "gen_hydro_border": "#1B5E20",
    "demand": "#FCE4EC",
    "demand_border": "#880E4F",
    "battery": "#F3E5F5",
    "battery_border": "#4A148C",
    "converter": "#E0F7FA",
    "converter_border": "#006064",
    "junction": "#E8F5E9",
    "junction_border": "#1B5E20",
    "reservoir": "#E1F5FE",
    "reservoir_border": "#01579B",
    "reservoir_sm": "#E1F5FE",
    "reservoir_sm_border": "#4FC3F7",
    "reservoir_md": "#81D4FA",
    "reservoir_md_border": "#0288D1",
    "reservoir_lg": "#039BE5",
    "reservoir_lg_border": "#01579B",
    "turbine": "#E8F5E9",
    "turbine_border": "#1B5E20",
    "flow": "#E1F5FE",
    "flow_border": "#0277BD",
    "filtration": "#ECEFF1",
    "filtration_border": "#546E7A",
    "reserve_zone": "#FFF8E1",
    "reserve_zone_border": "#F57F17",
    "gen_profile": "#F3E5F5",
    "gen_profile_border": "#7B1FA2",
    "dem_profile": "#FCE4EC",
    "dem_profile_border": "#AD1457",
    "line_edge": "#37474F",
    "waterway_edge": "#0277BD",
    "bat_link_edge": "#4A148C",
    "efficiency_edge": "#BF360C",
    "reserve_edge": "#F57F17",
    "profile_edge": "#95A5A6",
}

# Voltage-based line coloring: maps voltage ranges to (color, width).
# Higher voltages get darker colors and wider lines.
_LINE_VOLTAGE_BANDS: list[tuple[float, str, float]] = [
    # (min_kv, color, width)
    (500.0, "#1A237E", 4.0),  # 500+ kV: dark indigo, extra wide
    (345.0, "#283593", 3.5),  # 345 kV: indigo
    (220.0, "#1565C0", 3.0),  # 220 kV: dark blue
    (154.0, "#1976D2", 2.5),  # 154 kV: blue
    (110.0, "#1E88E5", 2.0),  # 110 kV: medium blue
    (66.0, "#42A5F5", 1.5),  # 66 kV: light blue
    (33.0, "#64B5F6", 1.2),  # 33 kV: pale blue
    (0.0, "#90A4AE", 1.0),  # unknown/LV: gray, thin
]


def _reservoir_intensity(rel: float) -> str:
    """Return an intensity suffix based on relative reservoir capacity.

    The suffix maps to palette entries ``reservoir_sm``, ``reservoir_md``,
    ``reservoir_lg`` with progressively darker blue fills.
    """
    if rel >= 0.66:
        return "lg"
    if rel >= 0.33:
        return "md"
    return "sm"


def _line_color_width(kv: float) -> tuple[str, float]:
    """Return (color, width) for a line based on its voltage level."""
    for min_kv, color, width in _LINE_VOLTAGE_BANDS:
        if kv >= min_kv:
            return color, width
    return _PALETTE["line_edge"], 1.0


# Human-readable labels for each node kind (used in SVG and HTML legends)
_LEGEND_LABELS: dict[str, str] = {
    "bus": "Bus",
    "gen": "Generator (thermal)",
    "gen_solar": "Solar/Wind",
    "gen_hydro": "Hydro generator",
    "demand": "Demand",
    "battery": "Battery",
    "converter": "Converter",
    "junction": "Junction",
    "reservoir": "Reservoir",
    "reservoir_sm": "Reservoir (small)",
    "reservoir_md": "Reservoir (medium)",
    "reservoir_lg": "Reservoir (large)",
    "turbine": "Turbine",
    "flow": "Flow",
    "filtration": "Filtration",
    "reserve_zone": "Reserve zone",
    "gen_profile": "Generator profile",
    "dem_profile": "Demand profile",
}

_MM_SHAPES: dict[str, tuple[str, str]] = {
    "bus": ("[", "]"),
    "gen": ("[/", "\\]"),
    "gen_solar": ("[/", "\\]"),
    "gen_hydro": ("[/", "\\]"),
    "demand": ("[\\", "/]"),
    "battery": ("[(", ")]"),
    "converter": ("([", "])"),
    "junction": ("{{", "}}"),
    "reservoir": ("[/", "/]"),
    "turbine": ("{", "}"),
    "flow": ("[", "]"),
    "filtration": ("[", "]"),
}

_MM_STYLES: dict[str, str] = {
    "bus": "fill:#D6EAF8,stroke:#1A5276,color:#1C2833",
    "gen": "fill:#FEF9E7,stroke:#E67E22,color:#1C2833",
    "gen_solar": "fill:#FDEBD0,stroke:#F39C12,color:#1C2833",
    "gen_hydro": "fill:#D1F2EB,stroke:#1E8449,color:#1C2833",
    "demand": "fill:#FADBD8,stroke:#C0392B,color:#1C2833",
    "battery": "fill:#F4ECF7,stroke:#7D3C98,color:#1C2833",
    "converter": "fill:#D6EAF8,stroke:#1F618D,color:#1C2833",
    "junction": "fill:#EAFAF1,stroke:#1E8449,color:#1C2833",
    "reservoir": "fill:#EBF5FB,stroke:#1A5276,color:#1C2833",
    "turbine": "fill:#D1F2EB,stroke:#1E8449,color:#1C2833",
    "flow": "fill:#EAF2FF,stroke:#2980B9,color:#1C2833",
    "filtration": "fill:#EAECEE,stroke:#717D7E,color:#1C2833",
}

_MM_ICONS: dict[str, str] = {
    "bus": "🔌",
    "gen": "⚡",
    "gen_solar": "☀️",
    "gen_hydro": "💧",
    "demand": "📊",
    "battery": "🔋",
    "converter": "🔄",
    "junction": "🔵",
    "reservoir": "🏞️",
    "turbine": "⚙️",
    "flow": "🌊",
    "filtration": "🔽",
    # aggregated generator nodes inherit the type icon
    "gen_wind": "🌬️",
    "gen_nuclear": "☢️",
    "gen_gas": "🔥",
}

_FA_CDN = "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.2/css/all.min.css"

_PYVIS_COLORS: dict[str, dict] = {
    "bus": {"background": "#D6EAF8", "border": "#1A5276"},
    "gen": {"background": "#FEF9E7", "border": "#E67E22"},
    "gen_solar": {"background": "#FDEBD0", "border": "#F39C12"},
    "gen_hydro": {"background": "#D1F2EB", "border": "#1E8449"},
    "demand": {"background": "#FADBD8", "border": "#C0392B"},
    "battery": {"background": "#F4ECF7", "border": "#7D3C98"},
    "converter": {"background": "#D6EAF8", "border": "#1F618D"},
    "junction": {"background": "#EAFAF1", "border": "#1E8449"},
    "reservoir": {"background": "#EBF5FB", "border": "#1A5276"},
    "reservoir_sm": {"background": "#E1F5FE", "border": "#4FC3F7"},
    "reservoir_md": {"background": "#81D4FA", "border": "#0288D1"},
    "reservoir_lg": {"background": "#039BE5", "border": "#01579B"},
    "turbine": {"background": "#D1F2EB", "border": "#1E8449"},
    "flow": {"background": "#EAF2FF", "border": "#2980B9"},
    "filtration": {"background": "#EAECEE", "border": "#717D7E"},
    "reserve_zone": {"background": "#FFF8E1", "border": "#F57F17"},
    "gen_profile": {"background": "#F3E5F5", "border": "#7B1FA2"},
    "dem_profile": {"background": "#FCE4EC", "border": "#AD1457"},
}

_PYVIS_SHAPE_MAP: dict[str, str] = {
    "bus": "square",
    "gen": "triangle",
    "gen_solar": "triangle",
    "gen_hydro": "triangle",
    "demand": "triangleDown",
    "battery": "database",
    "converter": "ellipse",
    "junction": "hexagon",
    "reservoir": "box",
    "reservoir_sm": "box",
    "reservoir_md": "box",
    "reservoir_lg": "box",
    "turbine": "diamond",
    "flow": "dot",
    "filtration": "dot",
    "reserve_zone": "star",
    "gen_profile": "dot",
    "dem_profile": "dot",
}

_PYVIS_SIZE_MAP: dict[str, int] = {
    "bus": 30,
    "gen": 24,
    "gen_solar": 24,
    "gen_hydro": 24,
    "demand": 24,
    "battery": 30,
    "converter": 20,
    "junction": 28,
    "reservoir": 28,
    "turbine": 22,
    "flow": 16,
    "filtration": 14,
}


# ---------------------------------------------------------------------------
# Generator-type classification
# ---------------------------------------------------------------------------

# _GEN_TYPE_META is imported from _classify (canonical definition lives there)


# Thin wrappers delegating to _classify module (preserves private-name API)
_turbine_gen_refs = _turbine_gen_refs_impl
_turbine_waterway_refs = _turbine_waterway_refs_impl
_efficiency_turbine_pairs = _efficiency_turbine_pairs_impl
_classify_gen = _classify_gen_impl
_gen_pmax = _gen_pmax_impl
_elem_name = _elem_name_impl


# ---------------------------------------------------------------------------
# FilterOptions, Node, Edge, GraphModel — imported from _graph_model
# ---------------------------------------------------------------------------

# (FilterOptions, Node, Edge, GraphModel imported at top of file)

# ---------------------------------------------------------------------------
# Voltage-level bus reduction — imported from _data_utils
# ---------------------------------------------------------------------------

_build_voltage_map = _build_voltage_map_impl
_count_visible_buses = _count_visible_buses_impl
_resolve_bus_ref = _resolve_bus_ref_impl
# auto_voltage_threshold imported directly at top of file


# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------


_scalar = _scalar_impl
_resolve = _resolve_impl


# ---------------------------------------------------------------------------
# SVG icons for generator types — used in Graphviz HTML labels and HTML output
# ---------------------------------------------------------------------------

_GEN_TYPE_ICON_SVG: dict[str, str] = {}

_GEN_TYPE_ICON_SVG[
    "thermal"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><radialGradient id="rg" cx="50%" cy="60%" r="55%">
    <stop offset="0%" stop-color="#FEF9E7"/><stop offset="100%" stop-color="#F0B27A"/>
  </radialGradient></defs>
  <circle cx="24" cy="28" r="18" fill="url(#rg)" stroke="#E67E22" stroke-width="2.5"/>
  <path d="M9 28 C12 22 15 22 18 28 C21 34 24 34 27 28 C30 22 33 22 36 28"
        fill="none" stroke="#E67E22" stroke-width="2.2" stroke-linecap="round"/>
  <rect x="20" y="6" width="8" height="14" rx="2" fill="#BDC3C7" stroke="#7F8C8D" stroke-width="1.5"/>
  <rect x="17" y="3" width="14" height="5" rx="1" fill="#95A5A6"/>
  <path d="M22 6 Q22 1 24 1 Q26 1 26 6" fill="#E74C3C" opacity="0.7"/>
  <line x1="24" y1="8" x2="24" y2="12" stroke="#E74C3C" stroke-width="2"/>
  <circle cx="24" cy="46" r="2" fill="#7F8C8D"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "hydro"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs>
    <linearGradient id="dam" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="#BDC3C7"/><stop offset="100%" stop-color="#7F8C8D"/>
    </linearGradient>
    <linearGradient id="wtr" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="#AED6F1"/><stop offset="100%" stop-color="#2980B9"/>
    </linearGradient>
  </defs>
  <polygon points="4,44 10,8 38,8 44,44" fill="url(#dam)" stroke="#5D6D7E" stroke-width="2"/>
  <polygon points="10,8 38,8 36,44 12,44" fill="url(#wtr)" opacity="0.8"/>
  <path d="M13 22 Q18 18 23 22 Q28 26 33 22"
        fill="none" stroke="white" stroke-width="1.8" opacity="0.9"/>
  <circle cx="24" cy="36" r="5" fill="#1A5276" stroke="white" stroke-width="1.5"/>
  <text x="24" y="40" text-anchor="middle" font-family="Arial" font-weight="bold"
        font-size="7" fill="white">G</text>
  <line x1="24" y1="41" x2="24" y2="46" stroke="#1A5276" stroke-width="2.5"/>
  <circle cx="24" cy="47" r="2" fill="#1A5276"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "solar"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="pv" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#FDEBD0"/><stop offset="100%" stop-color="#F8C471"/>
  </linearGradient></defs>
  <g stroke="#F39C12" stroke-width="2.2" stroke-linecap="round">
    <line x1="24" y1="4" x2="24" y2="10"/>
    <line x1="24" y1="38" x2="24" y2="44"/>
    <line x1="4" y1="24" x2="10" y2="24"/>
    <line x1="38" y1="24" x2="44" y2="24"/>
    <line x1="9" y1="9" x2="13" y2="13"/>
    <line x1="35" y1="35" x2="39" y2="39"/>
    <line x1="39" y1="9" x2="35" y2="13"/>
    <line x1="9" y1="39" x2="13" y2="35"/>
  </g>
  <rect x="10" y="16" width="28" height="16" rx="2"
        fill="url(#pv)" stroke="#E67E22" stroke-width="2"/>
  <line x1="19" y1="16" x2="19" y2="32" stroke="#E67E22" stroke-width="1" opacity="0.6"/>
  <line x1="29" y1="16" x2="29" y2="32" stroke="#E67E22" stroke-width="1" opacity="0.6"/>
  <line x1="10" y1="24" x2="38" y2="24" stroke="#E67E22" stroke-width="1" opacity="0.6"/>
  <circle cx="24" cy="24" r="3.5" fill="#F39C12" opacity="0.85"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "wind"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="wnd" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#EBF5FB"/><stop offset="100%" stop-color="#5DADE2"/>
  </linearGradient></defs>
  <polygon points="22,44 26,44 25,20 23,20" fill="#BDC3C7" stroke="#95A5A6" stroke-width="1"/>
  <ellipse cx="24" cy="20" rx="4" ry="3" fill="#7F8C8D"/>
  <path d="M24 20 Q20 14 18 8 Q22 11 24 20" fill="url(#wnd)" stroke="#2980B9" stroke-width="1.2"/>
  <path d="M24 20 Q32 18 39 17 Q36 21 24 20" fill="url(#wnd)" stroke="#2980B9" stroke-width="1.2"/>
  <path d="M24 20 Q22 28 20 35 Q18 31 24 20" fill="url(#wnd)" stroke="#2980B9" stroke-width="1.2"/>
  <circle cx="24" cy="20" r="2.5" fill="#566573"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "battery_discharge"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="batt" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#E8DAEF"/><stop offset="100%" stop-color="#9B59B6"/>
  </linearGradient></defs>
  <rect x="5" y="13" width="38" height="24" rx="3" fill="url(#batt)" stroke="#7D3C98" stroke-width="2.5"/>
  <rect x="19" y="8" width="10" height="5" rx="1" fill="#7D3C98"/>
  <rect x="10" y="17" width="5" height="16" rx="1" fill="#7D3C98"/>
  <rect x="18" y="17" width="5" height="16" rx="1" fill="#7D3C98"/>
  <rect x="26" y="17" width="5" height="16" rx="1" fill="#7D3C98"/>
  <rect x="34" y="17" width="5" height="16" rx="1" fill="#D7BDE2"/>
  <polygon points="44,28 40,24 40,32" fill="#E74C3C"/>
  <line x1="40" y1="28" x2="47" y2="28" stroke="#E74C3C" stroke-width="2.5"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "nuclear"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><radialGradient id="nuc" cx="50%" cy="50%" r="50%">
    <stop offset="0%" stop-color="#FDEDEC"/><stop offset="100%" stop-color="#E74C3C"/>
  </radialGradient></defs>
  <circle cx="24" cy="24" r="20" fill="url(#nuc)" stroke="#C0392B" stroke-width="2.5"/>
  <circle cx="24" cy="24" r="4" fill="#C0392B"/>
  <path d="M24 20 Q18 10 12 8 Q16 14 24 20" fill="#C0392B" opacity="0.85"/>
  <path d="M28 26 Q38 22 40 16 Q34 20 28 26" fill="#C0392B" opacity="0.85"/>
  <path d="M20 26 Q10 30 8 36 Q14 32 20 26" fill="#C0392B" opacity="0.85"/>
  <circle cx="24" cy="24" r="2.5" fill="white"/>
</svg>"""

_GEN_TYPE_ICON_SVG[
    "gas"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="gas" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#FEF9E7"/><stop offset="100%" stop-color="#F8C471"/>
  </linearGradient></defs>
  <ellipse cx="24" cy="30" rx="16" ry="12" fill="url(#gas)" stroke="#E67E22" stroke-width="2.5"/>
  <path d="M24 22 Q30 16 36 14 Q32 20 24 22" fill="#E67E22" opacity="0.8"/>
  <path d="M24 22 Q18 28 16 34 Q12 28 24 22" fill="#E67E22" opacity="0.8"/>
  <circle cx="24" cy="22" r="3" fill="#D35400"/>
  <rect x="20" y="6" width="8" height="14" rx="3" fill="#95A5A6" stroke="#7F8C8D" stroke-width="1.5"/>
  <path d="M22 6 Q22 2 24 2 Q26 2 26 6" fill="#E74C3C" opacity="0.6"/>
</svg>"""


def _gen_type_icon_svg(gen_type: str) -> str:
    """Return the best SVG string for a generator type."""
    mapping = {
        "hydro": "hydro",
        "solar": "solar",
        "wind": "wind",
        "battery": "battery_discharge",
        "nuclear": "nuclear",
        "gas": "gas",
        "thermal": "thermal",
    }
    key = mapping.get(gen_type, "thermal")
    return _GEN_TYPE_ICON_SVG.get(key) or _ICON_SVG.get("gen", "")


_icon_key_for_type = _icon_key_for_type_impl
_dominant_kind = _dominant_kind_impl


# ---------------------------------------------------------------------------
# Topology builder
# ---------------------------------------------------------------------------


class TopologyBuilder:
    """Build a GraphModel from a gtopt JSON planning structure."""

    def __init__(self, planning, subsystem="full", opts=None):
        self.sys = planning.get("system", {})
        self.subsystem = subsystem
        self.opts = opts or FilterOptions()
        self.model = GraphModel(title=self.sys.get("name", "gtopt Network"))
        self._turb_refs = _turbine_gen_refs(self.sys)
        self._turb_way_refs = _turbine_waterway_refs(self.sys)
        self._eff_turb_refs = _efficiency_turbine_pairs(self.sys)
        self._vmap = _build_voltage_map(
            self.sys.get("bus_array", []),
            self.sys.get("line_array", []),
            self.opts.voltage_threshold,
        )
        self._bus_node_ids: set = set()
        self._eff_agg: str = "none"
        self._eff_vthresh: float = self.opts.voltage_threshold
        self._auto_info = None  # set by build() when aggregate="auto"
        self._focus_nids = None

    # ── Public accessors for build metadata ────────────────────────────────

    @property
    def eff_agg(self) -> str:
        """Effective aggregation mode resolved by :meth:`build`."""
        return self._eff_agg

    @property
    def eff_vthresh(self) -> float:
        """Effective voltage threshold resolved by :meth:`build`."""
        return self._eff_vthresh

    @property
    def auto_info(self):
        """(n_total, agg, vthresh) tuple set by auto mode, or *None*."""
        return self._auto_info

    @staticmethod
    def _make_id(prefix: str, item: dict) -> str:
        """Build a graph node ID from prefix, element name, and uid.

        Uses ``prefix_NAME_uid`` format to guarantee uniqueness across
        element types that may share the same name/uid (e.g. a turbine
        and its linked generator often share the same uid and name).
        """
        name = item.get("name")
        uid = item.get("uid")
        if name is not None:
            safe = str(name).replace(" ", "_").replace("-", "_")
            return f"{prefix}_{safe}_{uid}" if uid is not None else f"{prefix}_{safe}"
        return f"{prefix}_{uid}" if uid is not None else f"{prefix}_?"

    @staticmethod
    def _bid(b):
        return TopologyBuilder._make_id("bus", b)

    @staticmethod
    def _gid(g):
        return TopologyBuilder._make_id("gen", g)

    @staticmethod
    def _did(d):
        return TopologyBuilder._make_id("dem", d)

    @staticmethod
    def _batid(b):
        return TopologyBuilder._make_id("bat", b)

    @staticmethod
    def _cid(c):
        return TopologyBuilder._make_id("conv", c)

    @staticmethod
    def _jid(j):
        return TopologyBuilder._make_id("junc", j)

    @staticmethod
    def _rid(r):
        return TopologyBuilder._make_id("res", r)

    @staticmethod
    def _tid(t):
        return TopologyBuilder._make_id("turb", t)

    @staticmethod
    def _fid(f):
        return TopologyBuilder._make_id("flow", f)

    @staticmethod
    def _filtid(fi):
        return TopologyBuilder._make_id("filt", fi)

    @staticmethod
    def _rzid(rz):
        return TopologyBuilder._make_id("rzone", rz)

    @staticmethod
    def _rpid(rp):
        return TopologyBuilder._make_id("rprov", rp)

    @staticmethod
    def _gpid(gp):
        return TopologyBuilder._make_id("gprof", gp)

    @staticmethod
    def _dpid(dp):
        return TopologyBuilder._make_id("dprof", dp)

    def _find(self, arr_key, ref):
        return _resolve(self.sys.get(arr_key, []), ref)

    def _find_node_id(self, arr_key, ref, id_fn):
        item = self._find(arr_key, ref)
        return id_fn(item) if item else None

    def _bus_node_id(self, bus_ref):
        rep = _resolve_bus_ref(bus_ref, self._vmap)
        bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
        if bus is None:
            return None
        nid = self._bid(bus)
        return nid if nid in self._bus_node_ids else None

    def _gen_kind(self, gen):
        gt = _classify_gen(gen, self._turb_refs)
        return {"hydro": "gen_hydro", "solar": "gen_solar", "wind": "gen_solar"}.get(
            gt, "gen"
        )

    def _count_elements(self) -> int:
        """Return a rough total element count for auto-mode decisions."""
        s = self.sys
        return sum(
            len(s.get(k, []))
            for k in (
                "generator_array",
                "bus_array",
                "demand_array",
                "line_array",
                "battery_array",
                "converter_array",
                "junction_array",
                "waterway_array",
                "reservoir_array",
                "turbine_array",
                "flow_array",
                "filtration_array",
                "reservoir_efficiency_array",
            )
        )

    def build(self):
        agg = self.opts.aggregate
        vthresh = self.opts.voltage_threshold

        # ── Auto mode: choose strategy from element count ──────────────────
        if agg == "auto":
            n_total = self._count_elements()
            if n_total < _AUTO_NONE_THRESHOLD:
                agg = "none"
            elif n_total < _AUTO_BUS_THRESHOLD:
                agg = "bus"
            else:
                # Aggressive: aggregate by type AND apply a smart voltage
                # threshold chosen so that ≤ _AUTO_MAX_HV_BUSES buses remain.
                agg = "type"
                if vthresh == 0.0:
                    vthresh = auto_voltage_threshold(
                        self.sys.get("bus_array", []),
                        self.sys.get("line_array", []),
                        max_buses=_AUTO_MAX_HV_BUSES,
                    )
            self._auto_info = (n_total, agg, vthresh)

        # ── Explicit max_nodes escalation (overrides auto-resolved mode) ───
        if self.opts.max_nodes > 0:
            n_g = len(self.sys.get("generator_array", []))
            n_b = len(self.sys.get("bus_array", []))
            n_d = len(self.sys.get("demand_array", []))
            rough = n_b + n_g + n_d
            if agg == "none" and rough > self.opts.max_nodes:
                agg = "bus"
            if agg == "bus" and (n_b * 2 + n_d) > self.opts.max_nodes:
                agg = "type"
            if agg == "type" and n_b * 6 > self.opts.max_nodes * 2:
                agg = "global"

        self._eff_agg = agg
        self._eff_vthresh = vthresh
        # Update the voltage map if auto-mode changed the threshold
        if vthresh != self.opts.voltage_threshold:
            self._vmap = _build_voltage_map(
                self.sys.get("bus_array", []),
                self.sys.get("line_array", []),
                vthresh,
            )
            self._bus_node_ids = set()  # reset; will be repopulated by _buses()

        self._focus_nids = self._compute_focus_set() if self.opts.focus_buses else None

        # Auto-hide hydro subsystem when no hydro elements exist
        if self.subsystem == "full":
            sys = self.sys
            has_hydro = any(
                sys.get(k)
                for k in (
                    "junction_array",
                    "waterway_array",
                    "reservoir_array",
                    "turbine_array",
                    "flow_array",
                    "filtration_array",
                )
            )
            if not has_hydro:
                self.subsystem = "electrical"
                logger.debug(
                    "No hydro elements found; switching to electrical subsystem"
                )

        s = self.subsystem
        if s in ("full", "electrical"):
            self._buses()
            if not self.opts.no_generators:
                self._generators()
            self._demands()
            self._lines()
            self._batteries()
            self._converters()
            self._generator_profiles()
            self._demand_profiles()
        if s in ("full", "hydro"):
            self._junctions()
            self._waterways()
            self._reservoirs()
            self._turbines()
            self._flows()
            self._filtrations()
            self._reservoir_efficiencies()
        if s == "full":
            self._reserve_zones()
            self._reserve_provisions()
        # Remove edges that reference nodes absent from the model (e.g. when
        # subsystem="hydro" skips _generators(), turbine→generator edges would
        # otherwise reference non-existent node IDs and crash pyvis/render_html).
        node_ids = {n.node_id for n in self.model.nodes}
        self.model.edges = [
            e for e in self.model.edges if e.src in node_ids and e.dst in node_ids
        ]
        if self.opts.hide_isolated:
            connected = {e.src for e in self.model.edges} | {
                e.dst for e in self.model.edges
            }
            self.model.nodes = [n for n in self.model.nodes if n.node_id in connected]
        return self.model

    def _compute_focus_set(self):
        adj = defaultdict(set)
        for line in self.sys.get("line_array", []):
            ab = self._find("bus_array", line.get("bus_a"))
            bb = self._find("bus_array", line.get("bus_b"))
            if ab and bb:
                a, b = self._bid(ab), self._bid(bb)
                adj[a].add(b)
                adj[b].add(a)
        seeds = set()
        for fb in self.opts.focus_buses:
            bus = self._find("bus_array", fb)
            if bus:
                seeds.add(self._bid(bus))
        visited, frontier = set(seeds), set(seeds)
        for _ in range(self.opts.focus_hops):
            nxt = set()
            for nid in frontier:
                for nb in adj.get(nid, set()):
                    if nb not in visited:
                        visited.add(nb)
                        nxt.add(nb)
            frontier = nxt
        return visited

    def _buses(self):
        seen = set()
        for bus in self.sys.get("bus_array", []):
            ref = bus.get("uid") if bus.get("uid") is not None else bus.get("name")
            rep = _resolve_bus_ref(ref, self._vmap)
            if rep != ref:
                continue
            nid = self._bid(bus)
            if nid in seen:
                continue
            seen.add(nid)
            if self._focus_nids is not None and nid not in self._focus_nids:
                continue
            v = f"\n{bus['voltage']} kV" if "voltage" in bus else ""
            name = _elem_name(bus)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=f"{name}{v}",
                    kind="bus",
                    cluster="electrical",
                    tooltip=f"Bus uid={bus.get('uid')} name={bus.get('name')}{v}",
                )
            )
            self._bus_node_ids.add(nid)

    def _generators(self):
        gens = self.sys.get("generator_array", [])
        ftypes = [t.lower() for t in self.opts.filter_types]
        if ftypes:
            gens = [g for g in gens if _classify_gen(g, self._turb_refs) in ftypes]
        agg = self._eff_agg
        if agg == "none":
            self._gen_individual(gens)
        elif agg == "bus":
            self._gen_agg_bus(gens)
        elif agg == "type":
            self._gen_agg_type(gens)
        else:
            self._gen_agg_global(gens)

    def _gen_individual(self, gens):
        for gen in gens:
            pmax = _scalar(gen.get("pmax") or gen.get("capacity"))
            gcost = _scalar(gen.get("gcost", "\u2014"))
            gt = _classify_gen(gen, self._turb_refs)
            kind = self._gen_kind(gen)
            name = _elem_name(gen)
            lbl = (
                f"{name}\n{pmax} MW"
                if self.opts.compact
                else f"{name}\n{pmax} MW  {gcost} $/MWh"
            )
            nid = self._gid(gen)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=f"Generator uid={gen.get('uid')} type={gt} pmax={pmax} gcost={gcost}",
                )
            )
            bus_id = self._bus_node_id(gen.get("bus"))
            if bus_id:
                self.model.add_edge(Edge(nid, bus_id, color=_PALETTE[f"{kind}_border"]))

    def _gen_agg_bus(self, gens):
        by_bus = defaultdict(list)
        for g in gens:
            by_bus[g.get("bus")].append(g)
        for bus_ref, grp in by_bus.items():
            bus_id = self._bus_node_id(bus_ref)
            if bus_id is None:
                continue
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            rep = _resolve_bus_ref(bus_ref, self._vmap)
            bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
            bname = (
                _elem_name(bus)
                if bus
                else (bus_ref if isinstance(bus_ref, str) else f"bus{bus_ref}")
            )
            types = [_classify_gen(g, self._turb_refs) for g in grp]
            kind = _dominant_kind(types)
            nid = f"agg_bus_{bus_ref}"
            lbl = f"{bname} generators\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=f"Agg. generators at {bname}: {len(grp)} units, {total:.0f} MW",
                )
            )
            self.model.add_edge(Edge(nid, bus_id, color=_PALETTE[f"{kind}_border"]))

    def _gen_agg_type(self, gens):
        by_bus_type = defaultdict(list)
        for g in gens:
            gt = _classify_gen(g, self._turb_refs)
            by_bus_type[(g.get("bus"), gt)].append(g)
        for (bus_ref, gt), grp in by_bus_type.items():
            bus_id = self._bus_node_id(bus_ref)
            if bus_id is None:
                continue
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            rep = _resolve_bus_ref(bus_ref, self._vmap)
            bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
            bname = (
                _elem_name(bus)
                if bus
                else (bus_ref if isinstance(bus_ref, str) else f"bus{bus_ref}")
            )
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, icon, palette_key = meta
            nid = f"agg_type_{bus_ref}_{gt}"
            lbl = f"{icon} {label} @ {bname}\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=palette_key,
                    cluster="electrical",
                    tooltip=f"{label} at {bname}: {len(grp)} units, {total:.0f} MW",
                )
            )
            border = _PALETTE.get(f"{palette_key}_border", _PALETTE["gen_border"])
            self.model.add_edge(Edge(nid, bus_id, color=border))

    def _gen_agg_global(self, gens):
        by_type = defaultdict(list)
        for g in gens:
            by_type[_classify_gen(g, self._turb_refs)].append(g)
        for gt, grp in by_type.items():
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, icon, palette_key = meta
            nid = f"agg_global_{gt}"
            lbl = f"{icon} {label}\n{len(grp)} units · {total:.0f} MW total"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=palette_key,
                    cluster="electrical",
                    tooltip=f"All {label}: {len(grp)} units, {total:.0f} MW",
                )
            )
            seen_buses: set = set()
            for gen in grp:
                bus_id = self._bus_node_id(gen.get("bus"))
                if bus_id and bus_id not in seen_buses:
                    seen_buses.add(bus_id)
                    border = _PALETTE.get(
                        f"{palette_key}_border", _PALETTE["gen_border"]
                    )
                    self.model.add_edge(Edge(nid, bus_id, color=border))

    def _demands(self):
        for dem in self.sys.get("demand_array", []):
            name = _elem_name(dem)
            lmax = _scalar(dem.get("lmax"))
            nid = self._did(dem)
            lbl = f"{name}" if self.opts.compact else f"{name}\n{lmax} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind="demand",
                    cluster="electrical",
                    tooltip=f"Demand uid={dem.get('uid')} name={dem.get('name')} lmax={lmax}",
                )
            )
            bus_id = self._bus_node_id(dem.get("bus"))
            if bus_id:
                self.model.add_edge(Edge(bus_id, nid, color=_PALETTE["demand_border"]))

    def _lines(self):
        seen_edges: set = set()
        for line in self.sys.get("line_array", []):
            a_ref = line.get("bus_a")
            b_ref = line.get("bus_b")
            rep_a = _resolve_bus_ref(a_ref, self._vmap)
            rep_b = _resolve_bus_ref(b_ref, self._vmap)
            if rep_a == rep_b and self.opts.voltage_threshold > 0:
                continue
            ba = self._find("bus_array", rep_a) or self._find("bus_array", a_ref)
            bb = self._find("bus_array", rep_b) or self._find("bus_array", b_ref)
            if not ba or not bb:
                continue
            aid, bid2 = self._bid(ba), self._bid(bb)
            if aid not in self._bus_node_ids or bid2 not in self._bus_node_ids:
                continue
            if aid == bid2:
                continue
            # De-duplicate parallel lines in voltage-aggregated mode
            edge_key = tuple(sorted([aid, bid2]))
            if self.opts.voltage_threshold > 0 and edge_key in seen_edges:
                continue
            seen_edges.add(edge_key)
            name = _elem_name(line)
            x = line.get("reactance", "")
            tmax = line.get("tmax_ab", line.get("tmax_ba", ""))

            # Derive voltage from connected buses for color/width
            va = ba.get("voltage", 0) if ba else 0
            vb = bb.get("voltage", 0) if bb else 0
            line_kv = max(float(va or 0), float(vb or 0))

            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [str(name)]
                if line_kv > 0:
                    parts.append(f"{line_kv:.0f} kV")
                if tmax:
                    parts.append(f"{_scalar(tmax)} MW")
                lbl = "\n".join(parts)

            color, width = _line_color_width(line_kv)
            self.model.add_edge(
                Edge(
                    src=aid,
                    dst=bid2,
                    label=lbl,
                    color=color,
                    directed=False,
                    weight=width,
                )
            )

    def _batteries(self):
        for bat in self.sys.get("battery_array", []):
            name = _elem_name(bat)
            emax = _scalar(bat.get("emax") or bat.get("capacity"))
            ein = bat.get("input_efficiency", "")
            eout = bat.get("output_efficiency", "")
            eff = f"\nη={ein}/{eout}" if ein else ""
            lbl = f"{name}" if self.opts.compact else f"{name}\n{emax} MWh{eff}"
            self.model.add_node(
                Node(
                    node_id=self._batid(bat),
                    label=lbl,
                    kind="battery",
                    cluster="electrical",
                    tooltip=f"Battery uid={bat.get('uid')} emax={emax}",
                )
            )

    def _converters(self):
        for conv in self.sys.get("converter_array", []):
            name = _elem_name(conv)
            cap = _scalar(conv.get("capacity"))
            cid = self._cid(conv)
            lbl = f"{name}" if self.opts.compact else f"{name}\n{cap} MW"
            self.model.add_node(
                Node(
                    node_id=cid,
                    label=lbl,
                    kind="converter",
                    cluster="electrical",
                    tooltip=f"Converter uid={conv.get('uid')} cap={cap}",
                )
            )
            bat_id = self._find_node_id(
                "battery_array", conv.get("battery"), self._batid
            )
            gen_id = self._find_node_id(
                "generator_array", conv.get("generator"), self._gid
            )
            dem_id = self._find_node_id("demand_array", conv.get("demand"), self._did)
            if bat_id:
                self.model.add_edge(
                    Edge(
                        bat_id,
                        cid,
                        label="stored\nenergy",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        cid,
                        gen_id,
                        label="discharge",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )
            if dem_id:
                self.model.add_edge(
                    Edge(
                        dem_id,
                        cid,
                        label="charge",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )

    def _generator_profiles(self):
        """Draw generator profile nodes linked to their generators."""
        for gp in self.sys.get("generator_profile_array", []):
            name = _elem_name(gp)
            gpid = self._gpid(gp)
            profile = gp.get("profile", "")
            plbl = str(profile)[:20] if isinstance(profile, str) else "inline"
            lbl = str(name) if self.opts.compact else f"[GenProfile] {name}\n{plbl}"
            self.model.add_node(
                Node(
                    node_id=gpid,
                    label=lbl,
                    kind="gen_profile",
                    cluster="electrical",
                    tooltip=f"GeneratorProfile uid={gp.get('uid')} profile={plbl}",
                )
            )
            gen_id = self._find_node_id(
                "generator_array", gp.get("generator"), self._gid
            )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        gpid,
                        gen_id,
                        label="profile",
                        style="dotted",
                        color=_PALETTE.get("profile_edge", "#95A5A6"),
                    )
                )

    def _demand_profiles(self):
        """Draw demand profile nodes linked to their demands."""
        for dp in self.sys.get("demand_profile_array", []):
            name = _elem_name(dp)
            dpid = self._dpid(dp)
            profile = dp.get("profile", "")
            plbl = str(profile)[:20] if isinstance(profile, str) else "inline"
            lbl = str(name) if self.opts.compact else f"[DemProfile] {name}\n{plbl}"
            self.model.add_node(
                Node(
                    node_id=dpid,
                    label=lbl,
                    kind="dem_profile",
                    cluster="electrical",
                    tooltip=f"DemandProfile uid={dp.get('uid')} profile={plbl}",
                )
            )
            dem_id = self._find_node_id("demand_array", dp.get("demand"), self._did)
            if dem_id:
                self.model.add_edge(
                    Edge(
                        dpid,
                        dem_id,
                        label="profile",
                        style="dotted",
                        color=_PALETTE.get("profile_edge", "#95A5A6"),
                    )
                )

    def _junctions(self):
        for j in self.sys.get("junction_array", []):
            name = _elem_name(j)
            self.model.add_node(
                Node(
                    node_id=self._jid(j),
                    label=name,
                    kind="junction",
                    cluster="hydro",
                    tooltip=f"Junction uid={j.get('uid')} name={j.get('name')}",
                )
            )

    def _waterways(self):
        for w in self.sys.get("waterway_array", []):
            name = _elem_name(w)
            fmax = _scalar(w.get("fmax"))
            uid = w.get("uid")
            # Skip direct arc when a turbine already represents this waterway
            if uid in self._turb_way_refs or name in self._turb_way_refs:
                continue
            ja = self._find_node_id("junction_array", w.get("junction_a"), self._jid)
            jb = self._find_node_id("junction_array", w.get("junction_b"), self._jid)
            if ja and jb:
                lbl = str(name) if self.opts.compact else f"{name}\n≤{fmax} m³/s"
                self.model.add_edge(
                    Edge(
                        src=ja,
                        dst=jb,
                        label=lbl,
                        color=_PALETTE["waterway_edge"],
                        weight=float(w["fmax"]) if w.get("fmax") else 1.0,
                    )
                )

    def _reservoirs(self):
        # Collect all emax values to compute relative sizes and colors
        all_emax = []
        for r in self.sys.get("reservoir_array", []):
            val = _gen_pmax({"pmax": r.get("emax") or r.get("capacity")})
            if val > 0:
                all_emax.append(val)
        max_emax = max(all_emax) if all_emax else 1.0

        for r in self.sys.get("reservoir_array", []):
            name = _elem_name(r)
            emax_val = _gen_pmax({"pmax": r.get("emax") or r.get("capacity")})
            emax = _scalar(r.get("emax") or r.get("capacity"))
            lbl = str(name) if self.opts.compact else f"{name}\n{emax} dam³"
            # Relative size: 0.0 (smallest) to 1.0 (largest)
            rel = emax_val / max_emax if max_emax > 0 else 0.5
            node_size = 20.0 + 30.0 * rel
            self.model.add_node(
                Node(
                    node_id=self._rid(r),
                    label=lbl,
                    kind=f"reservoir_{_reservoir_intensity(rel)}",
                    cluster="hydro",
                    tooltip=f"Reservoir uid={r.get('uid')} emax={emax}",
                    size=node_size,
                )
            )
            junc_id = self._find_node_id("junction_array", r.get("junction"), self._jid)
            if junc_id:
                self.model.add_edge(
                    Edge(
                        self._rid(r),
                        junc_id,
                        style="dashed",
                        color=_PALETTE["reservoir_border"],
                    )
                )

    def _turbines(self):
        for t in self.sys.get("turbine_array", []):
            name = _elem_name(t)
            cap = _scalar(t.get("capacity"))
            cr = _scalar(t.get("conversion_rate"))
            tid = self._tid(t)
            lbl = (
                str(name)
                if self.opts.compact
                else f"[Turbine] {name}\n{cap} MW  cr={cr}"
            )
            self.model.add_node(
                Node(
                    node_id=tid,
                    label=lbl,
                    kind="turbine",
                    cluster="hydro",
                    tooltip=f"Turbine uid={t.get('uid')} cap={cap}",
                )
            )
            way = _resolve(self.sys.get("waterway_array", []), t.get("waterway"))
            if way:
                ja = self._find_node_id(
                    "junction_array", way.get("junction_a"), self._jid
                )
                jb = self._find_node_id(
                    "junction_array", way.get("junction_b"), self._jid
                )
                way_name = _elem_name(way)
                fmax = _scalar(way.get("fmax"))
                lbl_w = (
                    str(way_name) if self.opts.compact else f"{way_name}\n≤{fmax} m³/s"
                )
                if ja:
                    self.model.add_edge(
                        Edge(
                            ja,
                            tid,
                            label=lbl_w,
                            color=_PALETTE["waterway_edge"],
                        )
                    )
                if jb:
                    # Water-out edge carries no label: the waterway name
                    # is already shown on the water-in edge above.
                    self.model.add_edge(
                        Edge(
                            tid,
                            jb,
                            color=_PALETTE["waterway_edge"],
                        )
                    )
            gen_ref = t.get("generator")
            gen_id = self._find_node_id("generator_array", gen_ref, self._gid)
            if gen_id:
                # In hydro subsystem the generator node may not exist yet;
                # add it so the turbine → generator "power out" edge renders.
                if gen_id not in {n.node_id for n in self.model.nodes}:
                    gen = _resolve(self.sys.get("generator_array", []), gen_ref)
                    if gen:
                        gname = _elem_name(gen)
                        pmax = _scalar(gen.get("pmax") or gen.get("capacity"))
                        gtype = gen.get("type", "hydro")
                        glbl = (
                            str(gname)
                            if self.opts.compact
                            else f"[Gen {gtype}] {gname}\n{pmax} MW"
                        )
                        self.model.add_node(
                            Node(
                                node_id=gen_id,
                                label=glbl,
                                kind="gen_hydro",
                                cluster="hydro",
                                tooltip=f"Generator uid={gen.get('uid')} type={gtype} pmax={pmax}",
                            )
                        )
                        # Connect generator to its bus (auto-create bus
                        # node if needed, e.g. in hydro-only subsystem)
                        bus_ref = gen.get("bus")
                        if bus_ref is not None:
                            bus = self._find("bus_array", bus_ref)
                            if bus:
                                bid = self._bid(bus)
                                if bid not in {n.node_id for n in self.model.nodes}:
                                    bname = _elem_name(bus)
                                    bv = (
                                        f"\n{bus['voltage']} kV"
                                        if "voltage" in bus
                                        else ""
                                    )
                                    self.model.add_node(
                                        Node(
                                            node_id=bid,
                                            label=f"{bname}{bv}",
                                            kind="bus",
                                            cluster="electrical",
                                        )
                                    )
                                self.model.add_edge(
                                    Edge(
                                        gen_id,
                                        bid,
                                        color=_PALETTE["gen_hydro_border"],
                                    )
                                )
                self.model.add_edge(
                    Edge(
                        tid,
                        gen_id,
                        label="power out",
                        color=_PALETTE["gen_hydro_border"],
                        style="dashed",
                    )
                )
            # Draw main_reservoir → turbine edge when not already covered
            # by a reservoir_efficiency_array entry (to avoid duplication).
            uid = t.get("uid")
            name_ref = t.get("name")
            if uid not in self._eff_turb_refs and name_ref not in self._eff_turb_refs:
                res_id = self._find_node_id(
                    "reservoir_array", t.get("main_reservoir"), self._rid
                )
                if res_id:
                    lbl_e = "" if self.opts.compact else "head"
                    self.model.add_edge(
                        Edge(
                            res_id,
                            tid,
                            label=lbl_e,
                            style="dashed",
                            color=_PALETTE["efficiency_edge"],
                        )
                    )

    def _flows(self):
        for f in self.sys.get("flow_array", []):
            name = _elem_name(f)
            disc = _scalar(f.get("discharge"))
            direction = f.get("direction", 1)
            fid = self._fid(f)
            lbl = str(name) if self.opts.compact else f"{name}\n{disc} m³/s"
            self.model.add_node(
                Node(
                    node_id=fid,
                    label=lbl,
                    kind="flow",
                    cluster="hydro",
                    tooltip=f"Flow uid={f.get('uid')} discharge={disc}",
                )
            )
            junc_id = self._find_node_id("junction_array", f.get("junction"), self._jid)
            if junc_id:
                src, dst = (fid, junc_id) if direction >= 0 else (junc_id, fid)
                self.model.add_edge(Edge(src, dst, color=_PALETTE["waterway_edge"]))

    def _filtrations(self):
        for fi in self.sys.get("filtration_array", []):
            name = _elem_name(fi)
            fiid = self._filtid(fi)
            lbl = str(name) if self.opts.compact else f"{name}\n(filtration)"
            self.model.add_node(
                Node(
                    node_id=fiid,
                    label=lbl,
                    kind="filtration",
                    cluster="hydro",
                    tooltip=f"Filtration uid={fi.get('uid')} name={fi.get('name')}",
                )
            )
            wway = _resolve(self.sys.get("waterway_array", []), fi.get("waterway"))
            res_id = self._find_node_id(
                "reservoir_array", fi.get("reservoir"), self._rid
            )
            if wway:
                ja = self._find_node_id(
                    "junction_array", wway.get("junction_a"), self._jid
                )
                if ja:
                    self.model.add_edge(
                        Edge(
                            ja,
                            fiid,
                            style="dotted",
                            color=_PALETTE["filtration_border"],
                        )
                    )
            if res_id:
                self.model.add_edge(
                    Edge(
                        fiid,
                        res_id,
                        style="dotted",
                        color=_PALETTE["filtration_border"],
                    )
                )

    def _reservoir_efficiencies(self):
        """Draw turbine-reservoir efficiency relationships.

        Each entry in ``reservoir_efficiency_array`` associates a turbine with
        a reservoir whose volume drives the turbine's conversion rate (hydraulic
        head effect).  The relationship is drawn as a dashed edge
        ``reservoir → turbine`` coloured with ``efficiency_edge``.
        """
        for e in self.sys.get("reservoir_efficiency_array", []):
            res_id = self._find_node_id(
                "reservoir_array", e.get("reservoir"), self._rid
            )
            turb_id = self._find_node_id("turbine_array", e.get("turbine"), self._tid)
            if res_id and turb_id:
                lbl = "" if self.opts.compact else "head"
                self.model.add_edge(
                    Edge(
                        res_id,
                        turb_id,
                        label=lbl,
                        style="dashed",
                        color=_PALETTE["efficiency_edge"],
                    )
                )

    def _reserve_zones(self):
        """Draw reserve zone nodes as dotted-outline clusters."""
        for rz in self.sys.get("reserve_zone_array", []):
            name = _elem_name(rz)
            rzid = self._rzid(rz)
            lbl = str(name) if self.opts.compact else f"[Reserve Zone] {name}"
            self.model.add_node(
                Node(
                    node_id=rzid,
                    label=lbl,
                    kind="reserve_zone",
                    cluster="electrical",
                    tooltip=f"ReserveZone uid={rz.get('uid')} name={rz.get('name')}",
                )
            )

    def _reserve_provisions(self):
        """Draw reserve provision edges: generator → reserve_zone."""
        for rp in self.sys.get("reserve_provision_array", []):
            gen_id = self._find_node_id(
                "generator_array", rp.get("generator"), self._gid
            )
            # reserve_zones is a colon-separated string of zone refs
            rz_str = rp.get("reserve_zones", "")
            if isinstance(rz_str, str) and rz_str:
                for rz_ref in rz_str.split(":"):
                    rz_ref = rz_ref.strip()
                    if not rz_ref:
                        continue
                    rz_id = self._find_node_id("reserve_zone_array", rz_ref, self._rzid)
                    if gen_id and rz_id:
                        self.model.add_edge(
                            Edge(
                                gen_id,
                                rz_id,
                                label="reserve",
                                style="dotted",
                                color=_PALETTE.get(
                                    "reserve_edge", _PALETTE["gen_border"]
                                ),
                            )
                        )
            elif isinstance(rz_str, (list, tuple)):
                for rz_ref in rz_str:
                    rz_id = self._find_node_id("reserve_zone_array", rz_ref, self._rzid)
                    if gen_id and rz_id:
                        self.model.add_edge(
                            Edge(
                                gen_id,
                                rz_id,
                                label="reserve",
                                style="dotted",
                                color=_PALETTE.get(
                                    "reserve_edge", _PALETTE["gen_border"]
                                ),
                            )
                        )


# ---------------------------------------------------------------------------
# Planning structure diagram — pure SVG generator
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
        "        +Options options",
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


# ---------------------------------------------------------------------------
# Image viewer helper
# ---------------------------------------------------------------------------


def _mermaid_to_html(mermaid_text: str, title: str = "gtopt Diagram") -> str:
    """Return a self-contained HTML page that renders *mermaid_text* in a browser.

    The markdown backtick fences (`` ```mermaid`` / `` ``` ``) are stripped
    automatically so the text may be passed with or without them.
    """
    lines = mermaid_text.splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    if lines and lines[-1].startswith("```"):
        lines = lines[:-1]
    mmd_inner = "\n".join(lines)

    return textwrap.dedent(f"""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
<style>
  body {{ font-family: Arial, sans-serif; background: #F4F6F7; margin: 0; padding: 20px; }}
  h1   {{ color: #1A252F; font-size: 22px; margin-bottom: 16px; }}
  .card {{ background: white; border: 1px solid #D5D8DC; border-radius: 10px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.07); padding: 16px; overflow-x: auto; }}
  .mermaid {{ text-align: center; }}
</style>
</head>
<body>
<h1>{title}</h1>
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


def _show_mermaid(mermaid_text: str, title: str = "gtopt Diagram") -> None:
    """Write *mermaid_text* as a temporary HTML file and open it in a browser."""
    html = _mermaid_to_html(mermaid_text, title=title)
    fd, tmp_path = tempfile.mkstemp(suffix=".html", prefix="gtopt_mermaid_")
    try:
        os.write(fd, html.encode("utf-8"))
    finally:
        os.close(fd)
    webbrowser.open(Path(tmp_path).as_uri())


def display_diagram(path: str, fmt: str) -> None:
    """Open *path* in a suitable viewer.

    Strategy (tried in order):
    1. For PNG: try ``PIL.Image`` (Pillow) ``show()``.
    2. For all formats: ``webbrowser.open()`` — works on Linux, macOS and
       Windows without any extra dependencies.
    """
    abs_path = str(Path(path).resolve())
    if fmt == "png":
        try:
            from PIL import Image  # noqa: PLC0415

            Image.open(abs_path).show()
            return
        except ImportError:
            pass  # fall through to webbrowser
    webbrowser.open(Path(abs_path).as_uri())


# ---------------------------------------------------------------------------
# Graphviz topology renderer
# ---------------------------------------------------------------------------

_GRAPHVIZ_INSTALL_MSG = (
    "Graphviz executables not found.\n"
    "Install the system package, e.g.:\n"
    "  sudo apt-get install graphviz    # Debian/Ubuntu\n"
    "  brew install graphviz            # macOS\n"
    "  winget install graphviz          # Windows"
)


# Graphviz native shape mapping (distinct shapes for each element type)
_GV_SHAPE_MAP: dict[str, str] = {
    "bus": "box",
    "gen": "invtriangle",
    "gen_solar": "invtriangle",
    "gen_hydro": "invtriangle",
    "demand": "triangle",
    "battery": "cylinder",
    "converter": "ellipse",
    "junction": "hexagon",
    "reservoir": "box3d",
    "reservoir_sm": "box3d",
    "reservoir_md": "box3d",
    "reservoir_lg": "box3d",
    "turbine": "diamond",
    "flow": "circle",
    "filtration": "circle",
    "reserve_zone": "star",
    "gen_profile": "note",
    "dem_profile": "note",
}


def _gv_node_attrs(node: Node, icon_path: Optional[str]) -> dict[str, str]:
    """Return Graphviz node attributes with native shapes and filled colors."""
    bg = _PALETTE.get(node.kind, "#FFF")
    border = _PALETTE.get(f"{node.kind}_border", "#333")
    lbl = node.label.replace("\n", "\\n")

    if icon_path:
        # Use image with HTML label (keeps TABLE for icon support)
        html_lbl = (
            node.label.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace("\n", "<BR/>")
        )
        return {
            "label": (
                f'<<TABLE BORDER="0" CELLBORDER="0" CELLSPACING="0" CELLPADDING="4">'
                f'<TR><TD ALIGN="CENTER"><IMG SRC="{icon_path}"/></TD></TR>'
                f'<TR><TD ALIGN="CENTER">'
                f'<FONT FACE="Arial" POINT-SIZE="9">{html_lbl}</FONT>'
                f"</TD></TR></TABLE>>"
            ),
            "shape": "none",
        }

    shape = _GV_SHAPE_MAP.get(node.kind, "ellipse")
    width = str(max(0.5, node.size / 30.0)) if node.size > 0 else ""
    attrs: dict[str, str] = {
        "label": lbl,
        "shape": shape,
        "style": "filled",
        "fillcolor": bg,
        "color": border,
        "fontname": "Arial",
        "fontsize": "9",
        "penwidth": "1.5",
    }
    if width:
        attrs["width"] = width
        attrs["height"] = width
    return attrs


def render_graphviz(
    model: GraphModel,
    fmt: str = "svg",
    output_path: Optional[str] = None,
    layout: str = "neato",
    use_clusters: bool = False,
) -> str:
    try:
        import graphviz as gv  # noqa: PLC0415
    except ImportError as err:
        raise SystemExit("pip install graphviz") from err

    is_directed = any(e.directed for e in model.edges)
    cls = gv.Digraph if is_directed else gv.Graph
    dot = cls(name=model.title, engine=layout, format=fmt if fmt != "dot" else "svg")
    dot.attr(
        label=f"<<B>{model.title}</B>>",
        labelloc="t",
        fontname="Arial",
        fontsize="14",
        bgcolor="white",
        pad="0.5",
        splines="curved",
        overlap="false",
        nodesep="0.6",
        ranksep="1.0",
    )
    dot.attr("node", margin="0.05,0.05", fontname="Arial", fontsize="9")
    dot.attr("edge", fontname="Arial", fontsize="9", penwidth="1.5")

    elec_nodes = {n.node_id for n in model.nodes if n.cluster == "electrical"}
    hydro_nodes = {n.node_id for n in model.nodes if n.cluster == "hydro"}
    has_both = bool(elec_nodes) and bool(hydro_nodes)

    def add_node(d, node: Node) -> None:
        icon = _icon_png_path(node.kind)
        attrs = _gv_node_attrs(node, icon)
        attrs["tooltip"] = node.tooltip or node.label
        d.node(node.node_id, **attrs)

    if has_both and use_clusters:
        with dot.subgraph(name="cluster_elec") as c:
            c.attr(
                label="Electrical Network",
                bgcolor="#F8FBFF",
                color="#AED6F1",
                style="rounded",
                fontname="Arial Bold",
            )
            for n in model.nodes:
                if n.cluster == "electrical":
                    add_node(c, n)
        with dot.subgraph(name="cluster_hydro") as c:
            c.attr(
                label="Hydro Cascade",
                bgcolor="#F0FFF4",
                color="#A9DFBF",
                style="rounded",
                fontname="Arial Bold",
            )
            for n in model.nodes:
                if n.cluster == "hydro":
                    add_node(c, n)
    else:
        for n in model.nodes:
            add_node(dot, n)

    for e in model.edges:
        attrs: dict = {}
        if e.color:
            attrs["color"] = e.color
        if e.label:
            attrs["label"] = e.label.replace("\n", "\\n")
        if e.style == "dashed":
            attrs.update(style="dashed", penwidth="1.2")
        elif e.style == "dotted":
            attrs.update(style="dotted", penwidth="1.0")
        if not e.directed:
            attrs["dir"] = "none"
            attrs["penwidth"] = str(max(1.5, min(4.0, 1.0 + e.weight / 100)))
        dot.edge(e.src, e.dst, **attrs)

    # Add legend as a subgraph when multiple node kinds are present
    kinds_in_model = {n.kind for n in model.nodes}
    if len(kinds_in_model) > 1:
        with dot.subgraph(name="cluster_legend") as legend:
            legend.attr(
                label="Legend",
                style="rounded",
                color="#CCCCCC",
                fontsize="10",
                fontname="Arial",
            )
            for kind in sorted(kinds_in_model):
                label_text = _LEGEND_LABELS.get(kind, kind)
                fill = _PALETTE.get(kind, "#FFFFFF")
                border = _PALETTE.get(f"{kind}_border", "#333333")
                legend.node(
                    f"legend_{kind}",
                    label=label_text,
                    shape="box",
                    style="filled",
                    fillcolor=fill,
                    color=border,
                    fontsize="8",
                    fontname="Arial",
                    width="0.1",
                    height="0.1",
                )

    if fmt == "dot":
        return dot.source

    if output_path:
        out = Path(output_path)
        try:
            rendered = dot.render(
                filename=str(out.stem),
                directory=str(out.parent) if str(out.parent) != "." else ".",
                format=fmt,
                cleanup=True,
            )
        except FileNotFoundError as err:
            raise SystemExit(_GRAPHVIZ_INSTALL_MSG) from err
        return rendered
    try:
        return dot.pipe(format=fmt).decode("utf-8", errors="replace")
    except FileNotFoundError as err:
        raise SystemExit(_GRAPHVIZ_INSTALL_MSG) from err


# ---------------------------------------------------------------------------
# Mermaid renderer
# ---------------------------------------------------------------------------


def render_mermaid(model: GraphModel, direction: str = "LR") -> str:
    lines = [
        "```mermaid",
        "---",
        f"title: {model.title}",
        "---",
        f"flowchart {direction}",
        "",
    ]

    for n in model.nodes:
        shapes = _MM_SHAPES.get(n.kind, ("[", "]"))
        icon = _MM_ICONS.get(n.kind, "")
        safe = n.node_id.replace("-", "_")
        lbl = (icon + " " + n.label).replace('"', "'").replace("\n", "<br/>")
        lines.append(f'    {safe}{shapes[0]}"{lbl}"{shapes[1]}')

    lines.append("")
    for e in model.edges:
        s = e.src.replace("-", "_")
        d = e.dst.replace("-", "_")
        lbl = e.label.replace('"', "'").replace("\n", " ") if e.label else ""
        if not e.directed:
            arrow = "---"
        elif e.style == "dashed":
            arrow = "-.->"
        elif e.style == "dotted":
            arrow = "...>"
        else:
            arrow = "-->"
        lines.append(f"    {s} {arrow}|{lbl}| {d}" if lbl else f"    {s} {arrow} {d}")

    lines.append("")
    kind_nodes: dict[str, list[str]] = {}
    for n in model.nodes:
        kind_nodes.setdefault(n.kind, []).append(n.node_id.replace("-", "_"))
    for kind, nids in kind_nodes.items():
        style = _MM_STYLES.get(kind, "fill:#FFF,stroke:#333")
        lines.append(f"    classDef cls_{kind} {style}")
        lines.append(f"    class {','.join(nids)} cls_{kind}")

    lines.append("```")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Public vis.js data export
# ---------------------------------------------------------------------------


def model_to_visjs(model: GraphModel) -> dict:
    """Convert a :class:`GraphModel` to a vis.js-compatible ``{nodes, edges}`` dict.

    This is the **public API** for converting a topology model to a format
    suitable for use with `vis-network <https://visjs.github.io/vis-network/>`_.
    It replaces the need to import the private ``_PYVIS_*`` constants directly.

    Returns a dict with two keys:

    ``nodes``
        A list of vis.js node objects, each with the fields ``id``, ``label``,
        ``title`` (tooltip), ``shape``, ``color`` (``{background, border}``),
        ``size``, ``kind``, and ``group``.

    ``edges``
        A list of vis.js edge objects with ``id``, ``from``, ``to``, ``label``,
        ``title``, ``dashes``, ``arrows``, ``color``, and ``width``.
    """
    vis_nodes = []
    for node in model.nodes:
        colors = _PYVIS_COLORS.get(
            node.kind, {"background": "#F0F0F0", "border": "#555"}
        )
        vis_nodes.append(
            {
                "id": node.node_id,
                "label": node.label,
                "title": node.tooltip or node.label,
                "shape": _PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                "color": colors,
                "size": node.size
                if node.size > 0
                else _PYVIS_SIZE_MAP.get(node.kind, 20),
                "kind": node.kind,
                "group": node.cluster or node.kind,
            }
        )

    vis_edges = []
    for i, edge in enumerate(model.edges):
        vis_edges.append(
            {
                "id": i,
                "from": edge.src,
                "to": edge.dst,
                "label": edge.label.replace("\n", " ") if edge.label else "",
                "title": edge.label.replace("\n", " ") if edge.label else "",
                "dashes": edge.style in ("dashed", "dotted"),
                "arrows": "to" if edge.directed else "",
                "color": {"color": edge.color or "#2C3E50", "opacity": 0.8},
                "width": max(1.0, min(4.0, 1.0 + edge.weight / 100)),
            }
        )

    return {"nodes": vis_nodes, "edges": vis_edges}


# ---------------------------------------------------------------------------
# Interactive HTML renderer (pyvis + font-awesome + custom icons)
# ---------------------------------------------------------------------------


def _legend_html(model: GraphModel) -> str:
    """Build an HTML legend that matches the actual vis.js node shapes and colors."""
    kinds = {n.kind for n in model.nodes}
    labels = {
        "bus": "Bus (electrical node)",
        "gen": "Generator (thermal)",
        "gen_solar": "Generator (solar/wind)",
        "gen_hydro": "Generator (hydro-linked)",
        "demand": "Demand / load",
        "battery": "Battery storage",
        "converter": "Power converter",
        "junction": "Hydraulic junction",
        "reservoir": "Reservoir / dam",
        "turbine": "Hydraulic turbine",
        "flow": "Inflow / outflow",
        "filtration": "Filtration / seepage",
        "reserve_zone": "Reserve zone",
        "gen_profile": "Generator profile",
        "dem_profile": "Demand profile",
    }
    # SVG path snippets that approximate vis.js node shapes
    _shape_svg = {
        "square": '<rect x="1" y="1" width="14" height="14"/>',
        "triangle": '<polygon points="8,1 15,15 1,15"/>',
        "triangleDown": '<polygon points="1,1 15,1 8,15"/>',
        "diamond": '<polygon points="8,1 15,8 8,15 1,8"/>',
        "hexagon": '<polygon points="4,1 12,1 15,8 12,15 4,15 1,8"/>',
        "box": '<rect x="1" y="3" width="14" height="10" rx="2"/>',
        "database": '<ellipse cx="8" cy="5" rx="7" ry="3"/>'
        '<rect x="1" y="5" width="14" height="7"/>'
        '<ellipse cx="8" cy="12" rx="7" ry="3"/>',
        "ellipse": '<ellipse cx="8" cy="8" rx="7" ry="6"/>',
        "dot": '<circle cx="8" cy="8" r="6"/>',
        "star": '<polygon points="8,1 10,6 15,6 11,10 13,15 8,12 3,15 5,10 1,6 6,6"/>',
    }
    entries = []
    for kind, lbl in labels.items():
        if kind not in kinds:
            continue
        colors = _PYVIS_COLORS.get(kind, {"background": "#F0F0F0", "border": "#555"})
        bg = colors["background"]
        bd = colors["border"]
        shape_name = _PYVIS_SHAPE_MAP.get(kind, "dot")
        svg_inner = _shape_svg.get(shape_name, _shape_svg["dot"])
        svg = (
            f'<svg width="16" height="16" viewBox="0 0 16 16"'
            f' xmlns="http://www.w3.org/2000/svg">'
            f'<g fill="{bg}" stroke="{bd}" stroke-width="1.5">'
            f"{svg_inner}</g></svg>"
        )
        entries.append(
            f'<div style="display:flex;align-items:center;gap:8px;margin:3px 0">'
            f"{svg}<span>{lbl}</span></div>"
        )
    return (
        '<div style="position:fixed;bottom:20px;right:20px;'
        "background:rgba(255,255,255,0.97);border:1px solid #CCC;border-radius:10px;"
        "padding:14px 18px;font-family:Arial;font-size:12px;"
        'box-shadow:3px 3px 10px rgba(0,0,0,0.12);z-index:9999;min-width:200px;">'
        '<b style="display:block;margin-bottom:8px;font-size:13px">Legend</b>'
        + "".join(entries)
        + "</div>"
    )


def render_html(model: GraphModel, output_path: str) -> str:
    try:
        from pyvis.network import Network  # noqa: PLC0415
    except ImportError as err:
        raise SystemExit("pip install pyvis") from err

    net = Network(
        height="750px",
        width="100%",
        directed=True,
        notebook=False,
        bgcolor="#FAFAFA",
        font_color="#1C2833",
    )
    net.set_options("""{
      "physics": {
        "enabled": true,
        "barnesHut": {
          "gravitationalConstant": -7000,
          "centralGravity": 0.25,
          "springLength": 160,
          "springConstant": 0.04,
          "damping": 0.09
        },
        "stabilization": {"iterations": 400, "updateInterval": 10}
      },
      "edges": {
        "smooth": {"type": "curvedCW", "roundness": 0.12},
        "font": {"size": 10, "face": "Arial",
                 "background": "rgba(255,255,255,0.8)"},
        "scaling": {"min": 1, "max": 4}
      },
      "nodes": {
        "font": {"face": "Arial", "size": 12},
        "borderWidth": 2,
        "shadow": {"enabled": true, "size": 4, "x": 2, "y": 2}
      },
      "interaction": {
        "hover": true, "tooltipDelay": 150,
        "navigationButtons": true, "keyboard": true,
        "multiselect": true
      }
    }""")

    for node in model.nodes:
        icon_path = _icon_png_path(node.kind)
        color = dict(
            _PYVIS_COLORS.get(node.kind, {"background": "#FFF", "border": "#333"})
        )
        color["highlight"] = {"background": color["background"], "border": "#E74C3C"}
        size = int(node.size) if node.size > 0 else _PYVIS_SIZE_MAP.get(node.kind, 20)

        if icon_path:
            net.add_node(
                node.node_id,
                label=node.label,
                title=node.tooltip or node.label,
                shape="image",
                image=f"file://{icon_path}",
                size=size + 8,
                font={"size": 11, "face": "Arial"},
                color=color,
            )
        else:
            net.add_node(
                node.node_id,
                label=node.label,
                title=node.tooltip or node.label,
                shape=_PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                color=color,
                size=size,
                font={"size": 11, "face": "Arial"},
            )

    for e in model.edges:
        dashes = e.style in ("dashed", "dotted")
        c = e.color or "#2C3E50"
        net.add_edge(
            e.src,
            e.dst,
            label=e.label.replace("\n", " ") if e.label else "",
            title=e.label.replace("\n", " ") if e.label else "",
            color={"color": c, "opacity": 0.80},
            dashes=dashes,
            arrows="to" if e.directed else "",
            width=max(1.0, min(4.0, 1.0 + e.weight / 100)) if not dashes else 1.0,
        )

    net.save_graph(output_path)
    html = Path(output_path).read_text(encoding="utf-8")
    fa = f'<link rel="stylesheet" href="{_FA_CDN}">\n'
    info = (
        f'<div style="position:fixed;top:0;left:0;right:0;background:#1A252F;'
        f"color:white;padding:8px 20px;font-family:Arial;font-size:13px;"
        f'z-index:10000;display:flex;align-items:center;gap:20px;">'
        f"<b>gtopt</b> \u2014 <span>{model.title}</span>"
        f'<span style="margin-left:auto;font-size:11px;opacity:0.7">'
        f"{len(model.nodes)} elements \u00b7 {len(model.edges)} connections</span>"
        f'</div><div style="margin-top:40px"></div>'
    )
    html = html.replace("</head>", fa + "</head>", 1)
    html = html.replace("<body>", f"<body>\n{info}", 1)
    html = html.replace("</body>", f"\n{_legend_html(model)}\n</body>", 1)
    Path(output_path).write_text(html, encoding="utf-8")
    return output_path


# ---------------------------------------------------------------------------
# Auto layout selection
# ---------------------------------------------------------------------------


def _auto_layout(model: GraphModel, subsystem: str) -> str:
    n = len(model.nodes)
    if subsystem == "hydro":
        return "dot"
    if n <= _LAYOUT_DOT_THRESHOLD:
        return "dot"
    if n <= _LAYOUT_NEATO_THRESHOLD:
        return "neato"
    if n <= _LAYOUT_FDP_THRESHOLD:
        return "fdp"
    return "sfdp"  # best for very large graphs


# ---------------------------------------------------------------------------
# Main CLI
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:  # noqa: C901,PLR0912,PLR0915
    parser = argparse.ArgumentParser(
        prog="gtopt_diagram",
        description=(
            "Generate electrical, hydro, and planning-structure diagrams "
            "from a gtopt JSON planning file."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
Diagram types (--diagram-type):
  topology   Network topology (buses, generators, lines, hydro elements, …)
  planning   Conceptual time-structure (blocks / stages / phases / scenarios)

Subsystems (topology, --subsystem):
  full        All elements together (default)
  electrical  Buses, generators, demands, lines, batteries, converters
  hydro       Junctions, waterways, reservoirs, turbines, flows, filtrations

Output formats (--format):
  svg, png, pdf, dot   Graphviz-rendered static images (requires graphviz)
  mermaid              Mermaid source for GitHub Markdown
  html                 Interactive vis.js browser diagram (requires pyvis)

Aggregation modes (--aggregate):
  auto    [DEFAULT] Automatically choose based on total element count:
            < 100  elements → none   (show everything individually)
            100-999 elements → bus   (one summary node per bus)
            ≥ 1000 elements  → type + --voltage-threshold 200
                               (aggressive: per-type nodes + fold LV buses)
  none    Show every generator individually (best for small cases)
  bus     One summary node per bus (counts + total MW)
  type    One node per (bus, generator-type) pair with type icon
  global  One node per generator type for the whole system

Generator visibility:
  --no-generators     Show only network topology (buses, lines, demands,
                      hydro elements) — no generator nodes at all.
                      Useful for large networks where generators clutter the view.

Other reduction options:
  --top-gens N        Keep only the top-N generators per bus by pmax
  --filter-type TYPE  Show only generators of TYPE (hydro/solar/wind/thermal/battery)
  --focus-bus NAME    Show only buses within N hops of NAME (repeat for multiple)
  --focus-hops N      Number of hops for --focus-bus (default: 2)
  --max-nodes N       Hard cap: escalate aggregation until node count ≤ N
  --voltage-threshold V   Lump buses and lines below V kV into their nearest
                          high-voltage neighbour (e.g. --voltage-threshold 200)
  --hide-isolated     Remove nodes with no connections
  --compact           Omit detail labels (show only names / counts)

Examples:
  # Auto mode (default) — picks the right aggregation for case size
  gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg          # <100: none
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o c2y.svg # ≥1000: type+smart threshold

  # Topology-only (no generators) — clean network diagram
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --no-generators -o topo.svg

  # Interactive HTML — explore with physics simulation
  gtopt_diagram cases/ieee_9b/ieee_9b.json --format html -o ieee9b.html

  # Force explicit mode
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
      --aggregate type --voltage-threshold 220 --format html -o case2y.html

  # Show only hydro generators in 2-hop neighbourhood of a bus
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
      --filter-type hydro --focus-bus Chapo220 --focus-hops 3 --format svg

  # Planning structure from a real case
  gtopt_diagram cases/c0/system_c0.json --diagram-type planning --format html

  # Mermaid snippet for GitHub Markdown
  gtopt_diagram cases/bat_4b/bat_4b.json --format mermaid
        """),
    )
    parser.add_argument(
        "json_file",
        nargs="?",
        help="gtopt JSON planning file (optional for planning diagrams)",
    )
    parser.add_argument(
        "--diagram-type",
        "-t",
        choices=["topology", "planning"],
        default="topology",
        help="Diagram type (default: topology)",
    )
    parser.add_argument(
        "--format",
        "-f",
        choices=["dot", "png", "svg", "pdf", "mermaid", "html"],
        default="svg",
        help="Output format (default: svg)",
    )
    parser.add_argument(
        "--output",
        "-o",
        default=None,
        help="Output file path (default: <case>_<type>.<ext>)",
    )
    parser.add_argument(
        "--subsystem",
        "-s",
        choices=["full", "electrical", "hydro"],
        default="full",
        help="Subsystem for topology diagrams (default: full)",
    )
    parser.add_argument(
        "--layout",
        "-L",
        choices=["dot", "neato", "fdp", "sfdp", "circo", "twopi"],
        default=None,
        help="Graphviz layout engine (auto-selected if omitted)",
    )
    parser.add_argument(
        "--direction",
        "-d",
        choices=["LR", "TD", "BT", "RL"],
        default="LR",
        help="Mermaid flowchart direction (default: LR)",
    )
    parser.add_argument(
        "--clusters",
        action="store_true",
        default=False,
        help="Group electrical/hydro in Graphviz sub-clusters (full topology)",
    )

    # ── Reduction / aggregation options ─────────────────────────────────────
    red = parser.add_argument_group(
        "reduction",
        "Options for simplifying large diagrams with many elements",
    )
    red.add_argument(
        "--aggregate",
        "-a",
        choices=["auto", "none", "bus", "type", "global"],
        default="auto",
        metavar="MODE",
        help=(
            "Aggregation mode (default: auto). "
            "auto=smart selection by element count (<100: none, 100-999: bus, "
            "≥1000: type+smart voltage threshold). "
            "none=individual elements, bus=per-bus summary, "
            "type=per-(bus,type) node, global=one node per type"
        ),
    )
    red.add_argument(
        "--no-generators",
        action="store_true",
        default=False,
        dest="no_generators",
        help=(
            "Omit all generator nodes — show only network topology "
            "(buses, lines, demands, hydro elements). "
            "Useful for very large cases where generators clutter the diagram."
        ),
    )
    red.add_argument(
        "--top-gens",
        "-g",
        type=int,
        default=0,
        metavar="N",
        help="Keep only the top-N generators per bus by pmax (0 = all)",
    )
    red.add_argument(
        "--filter-type",
        nargs="+",
        default=[],
        metavar="TYPE",
        dest="filter_types",
        help="Show only generators of these types: hydro solar wind thermal battery",
    )
    red.add_argument(
        "--focus-bus",
        nargs="+",
        default=[],
        metavar="BUS",
        dest="focus_buses",
        help="Show only elements reachable from these bus names",
    )
    red.add_argument(
        "--focus-generator",
        nargs="+",
        default=[],
        metavar="GEN",
        dest="focus_generators",
        help="Focus on the bus(es) connected to these generators (by name or uid)",
    )
    red.add_argument(
        "--focus-area",
        type=float,
        default=0.0,
        metavar="KV",
        help="Focus on buses at or above this voltage level (kV)",
    )
    red.add_argument(
        "--focus-hops",
        type=int,
        default=2,
        metavar="N",
        help="Number of line hops for --focus-bus (default: 2)",
    )
    red.add_argument(
        "--max-nodes",
        type=int,
        default=0,
        metavar="N",
        help="Auto-upgrade aggregation mode until node count ≤ N (0 = disabled)",
    )
    red.add_argument(
        "--voltage-threshold",
        type=float,
        default=0.0,
        metavar="KV",
        help=(
            "Lump buses below this voltage [kV] into their nearest HV neighbour. "
            "Lines between lumped buses are hidden. Buses without a voltage field "
            "are never lumped. (0 = disabled, e.g. --voltage-threshold 200)"
        ),
    )
    red.add_argument(
        "--hide-isolated",
        action="store_true",
        default=False,
        help="Remove nodes with no connections from the diagram",
    )
    red.add_argument(
        "--compact",
        action="store_true",
        default=False,
        help="Show only names/counts; omit pmax/gcost/reactance detail labels",
    )
    red.add_argument(
        "--show",
        action="store_true",
        default=False,
        help=(
            "Open the output file in a viewer after writing it. "
            "Uses Pillow (PIL) for PNG; webbrowser for SVG, PDF, HTML and Mermaid. "
            "Ignored for dot format written to stdout. "
            "Enabled automatically when no --output path is given."
        ),
    )
    parser.add_argument(
        "--palette",
        choices=["default", "colorblind"],
        default="default",
        help="Color palette for diagram elements (default: %(default)s)",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        metavar="LEVEL",
        help=(
            "logging verbosity: DEBUG, INFO, WARNING, ERROR, CRITICAL "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version="%(prog)s (gtopt-scripts)",
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    # ── Load JSON ────────────────────────────────────────────────────────────
    planning: dict = {}
    case_name = "gtopt"
    if args.json_file:
        jp = Path(args.json_file)
        if not jp.exists():
            logger.error("File not found: %s", jp)
            return 1
        with jp.open(encoding="utf-8") as fh:
            planning = json.load(fh)
        case_name = planning.get("system", {}).get("name", jp.stem)

    fmt = args.format
    sfx = {
        "dot": ".dot",
        "png": ".png",
        "svg": ".svg",
        "pdf": ".pdf",
        "mermaid": ".md",
        "html": ".html",
    }.get(fmt, f".{fmt}")
    out = args.output or f"{case_name}_{args.diagram_type}{sfx}"

    # Auto-enable show when no explicit output path was given (except for
    # text formats that are written to stdout, where there is nothing to open).
    show = args.show or (args.output is None and fmt not in ("mermaid", "dot"))

    # ── Planning structure diagram ───────────────────────────────────────────
    if args.diagram_type == "planning":
        if fmt == "mermaid":
            result = _build_planning_mermaid(planning)
            if args.output:
                Path(out).write_text(result, encoding="utf-8")
                logger.info("Mermaid planning diagram written to %s", out)
            if show:
                _show_mermaid(result, title=f"{case_name} — Planning Structure")
            elif not args.output:
                print(result)
            return 0
        if fmt == "html":
            ttl = (
                f"{case_name} — Planning Structure"
                if case_name != "gtopt"
                else "gtopt Planning Structure"
            )
            Path(out).write_text(
                _build_planning_html(planning, title=ttl), encoding="utf-8"
            )
            logger.info("Planning HTML written to %s", out)
            if show:
                display_diagram(out, fmt)
            return 0
        svg_src = _build_planning_svg(planning)
        if fmt in ("svg", "dot"):
            Path(out).write_text(svg_src, encoding="utf-8")
        else:
            try:
                import cairosvg  # noqa: PLC0415

                if fmt == "png":
                    cairosvg.svg2png(
                        bytestring=svg_src.encode(), write_to=out, scale=2.0
                    )
                elif fmt == "pdf":
                    cairosvg.svg2pdf(bytestring=svg_src.encode(), write_to=out)
                else:
                    Path(out).write_text(svg_src, encoding="utf-8")
            except ImportError:
                logger.warning("cairosvg not found; writing SVG instead")
                out = out.rsplit(".", 1)[0] + ".svg"
                Path(out).write_text(svg_src, encoding="utf-8")
        logger.info("Planning diagram written to %s", out)
        if show:
            display_diagram(out, fmt)
        return 0

    # ── Topology diagram ─────────────────────────────────────────────────────

    # Resolve --focus-generator to bus names
    extra_focus_buses: list[str] = list(args.focus_buses)
    sys_data = planning.get("system", {})
    if args.focus_generators:
        gen_array = sys_data.get("generator_array", [])
        gen_lookup: dict[str, dict] = {}
        for g in gen_array:
            gname = g.get("name")
            gid = g.get("uid")
            if gname is not None:
                gen_lookup[str(gname)] = g
            if gid is not None:
                gen_lookup[str(gid)] = g
        for gen_ref in args.focus_generators:
            g = gen_lookup.get(gen_ref)
            if g is None:
                logger.warning("--focus-generator: generator %r not found", gen_ref)
                continue
            bus_ref = g.get("bus")
            if bus_ref is not None:
                # Resolve bus uid to bus name
                bus_name = str(bus_ref)
                for bus in sys_data.get("bus_array", []):
                    if bus.get("uid") == bus_ref or str(bus.get("name")) == str(
                        bus_ref
                    ):
                        bus_name = str(bus.get("name", bus_ref))
                        break
                extra_focus_buses.append(bus_name)

    # Resolve --focus-area to bus names (buses at or above the given kV)
    if args.focus_area > 0:
        for bus in sys_data.get("bus_array", []):
            kv = bus.get("kv") or bus.get("voltage") or 0
            if float(kv) >= args.focus_area:
                bname = bus.get("name")
                if bname is not None:
                    extra_focus_buses.append(str(bname))

    # Apply --palette selection
    if args.palette == "colorblind":
        # Swap the module-level _PALETTE with colorblind variant
        _PALETTE.update(_PALETTE_COLORBLIND)

    # Print summary statistics if gtopt_check_json is available
    try:
        from gtopt_check_json._info import format_indicators  # noqa: PLC0415

        stats = format_indicators(
            planning,
            base_dir=str(Path(args.json_file).parent) if args.json_file else None,
        )
        print(stats, file=sys.stderr)
    except ImportError:
        pass

    opts = FilterOptions(
        aggregate=args.aggregate,
        no_generators=args.no_generators,
        top_gens=args.top_gens,
        filter_types=[t.lower() for t in args.filter_types],
        focus_buses=extra_focus_buses,
        focus_hops=args.focus_hops,
        max_nodes=args.max_nodes,
        voltage_threshold=args.voltage_threshold,
        hide_isolated=args.hide_isolated,
        compact=args.compact,
    )
    builder = TopologyBuilder(planning, subsystem=args.subsystem, opts=opts)
    model = builder.build()

    n_nodes = len(model.nodes)
    n_edges = len(model.edges)
    if n_nodes == 0:
        logger.warning("No elements found for the requested subsystem / filters.")
    else:
        agg_used = builder.eff_agg
        vt_used = builder.eff_vthresh
        flags: list[str] = []
        if agg_used != "none":
            flags.append(f"aggregate={agg_used}")
        if vt_used > 0:
            flags.append(f"voltage≥{vt_used:.0f} kV")
        if opts.no_generators:
            flags.append("no-generators")
        if opts.aggregate == "auto" and builder.auto_info:
            n_total, _, _ = builder.auto_info
            flags.append(f"auto({n_total} elements)")
        suffix = ("  [" + ", ".join(flags) + "]") if flags else ""
        logger.info("Diagram: %d nodes, %d edges%s", n_nodes, n_edges, suffix)

    if fmt == "mermaid":
        result = render_mermaid(model, direction=args.direction)
        if args.output:
            Path(out).write_text(result, encoding="utf-8")
            logger.info("Mermaid topology written to %s", out)
        if show:
            _show_mermaid(result, title=model.title)
        elif not args.output:
            print(result)
        return 0

    if fmt == "html":
        render_html(model, output_path=out)
        logger.info("Interactive HTML written to %s", out)
        if show:
            display_diagram(out, fmt)
        return 0

    layout = args.layout or _auto_layout(model, args.subsystem)
    clusters = args.clusters and args.subsystem == "full"

    if fmt == "dot":
        src = render_graphviz(model, fmt="dot", layout=layout, use_clusters=clusters)
        if args.output:
            Path(out).write_text(src, encoding="utf-8")
            logger.info("DOT source written to %s", out)
        else:
            print(src)
        return 0

    rendered = render_graphviz(
        model, fmt=fmt, output_path=out, layout=layout, use_clusters=clusters
    )
    logger.info("Diagram written to %s", rendered)
    if show:
        display_diagram(rendered, fmt)
    return 0


if __name__ == "__main__":
    sys.exit(main())

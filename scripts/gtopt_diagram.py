#!/usr/bin/env python3
"""
gtopt_diagram.py — Generate electrical, hydro and planning-structure diagrams
from gtopt JSON planning files.

Diagram types (--diagram-type)
-------------------------------
``topology``  — Network topology diagram (default)
``planning``  — Conceptual diagram of planning time structure
                (blocks, stages, phases, scenarios and their relationships)

Subsystems for topology diagrams (--subsystem)
-----------------------------------------------
``electrical`` — Buses, generators, demands, lines, batteries, converters
``hydro``      — Junctions, waterways, reservoirs, turbines, flows, filtrations
``full``       — Both subsystems together (default)

Output formats (--format)
--------------------------
``svg``      — Scalable SVG via Graphviz (recommended for docs)
``png``      — Rasterised PNG via Graphviz
``pdf``      — PDF via Graphviz
``dot``      — Raw Graphviz DOT source
``mermaid``  — Mermaid flowchart source (embeds in GitHub Markdown)
``html``     — Interactive HTML with vis.js / font-awesome (open in browser)

Dependencies
------------
    pip install graphviz pyvis cairosvg
    sudo apt-get install graphviz

Usage examples
--------------
    # Full topology SVG
    python3 scripts/gtopt_diagram.py cases/ieee_9b/ieee_9b.json -o ieee9b.svg

    # Electrical-only interactive HTML
    python3 scripts/gtopt_diagram.py cases/bat_4b/bat_4b.json \\
        --subsystem electrical --format html -o bat4b_elec.html

    # Hydro-only diagram
    python3 scripts/gtopt_diagram.py cases/hydro.json --subsystem hydro -o hydro.svg

    # Planning structure (no case file needed for generic diagram)
    python3 scripts/gtopt_diagram.py --diagram-type planning -o planning.svg

    # Planning structure from a real case
    python3 scripts/gtopt_diagram.py cases/c0/system_c0.json \\
        --diagram-type planning --format html -o c0_planning.html

    # Mermaid snippet for embedding in Markdown
    python3 scripts/gtopt_diagram.py cases/bat_4b/bat_4b.json --format mermaid
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
import tempfile
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

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

_ICON_SVG["filtration"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
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


def _icon_cache_dir() -> str:
    d = os.path.join(tempfile.gettempdir(), "gtopt_icons")
    os.makedirs(d, exist_ok=True)
    return d


def _icon_png_path(kind: str) -> Optional[str]:
    """Return absolute path to a cached PNG icon, creating it if needed."""
    if kind not in _ICON_SVG:
        return None
    if kind in _ICON_CACHE:
        return _ICON_CACHE[kind]
    try:
        import cairosvg  # noqa: PLC0415
    except ImportError:
        return None
    cache_dir = _icon_cache_dir()
    svg_src   = _ICON_SVG[kind]
    sig       = hashlib.md5(svg_src.encode()).hexdigest()[:8]
    png_path  = os.path.join(cache_dir, f"icon_{kind}_{sig}.png")
    if not os.path.exists(png_path):
        png_data = cairosvg.svg2png(bytestring=svg_src.encode(),
                                    output_width=48, output_height=48)
        with open(png_path, "wb") as fh:
            fh.write(png_data)
    _ICON_CACHE[kind] = png_path
    return png_path


def _icon_b64_uri(kind: str) -> str:
    """Return a base64 data URI for the SVG icon (for HTML embedding)."""
    svg = _ICON_SVG.get(kind, "")
    return "data:image/svg+xml;base64," + base64.b64encode(svg.encode()).decode() if svg else ""


# ---------------------------------------------------------------------------
# Colour palette
# ---------------------------------------------------------------------------

_PALETTE: dict[str, str] = {
    "bus":              "#D6EAF8", "bus_border":             "#1A5276",
    "gen":              "#FEF9E7", "gen_border":             "#E67E22",
    "gen_solar":        "#FDEBD0", "gen_solar_border":       "#F39C12",
    "gen_hydro":        "#D1F2EB", "gen_hydro_border":       "#1E8449",
    "demand":           "#FADBD8", "demand_border":          "#C0392B",
    "battery":          "#F4ECF7", "battery_border":         "#7D3C98",
    "converter":        "#D6EAF8", "converter_border":       "#1F618D",
    "junction":         "#EAFAF1", "junction_border":        "#1E8449",
    "reservoir":        "#EBF5FB", "reservoir_border":       "#1A5276",
    "turbine":          "#D1F2EB", "turbine_border":         "#1E8449",
    "flow":             "#D6EAF8", "flow_border":            "#2980B9",
    "filtration":       "#EAECEE", "filtration_border":      "#717D7E",
    "line_edge":        "#2C3E50",
    "waterway_edge":    "#2980B9",
    "bat_link_edge":    "#7D3C98",
}

_MM_SHAPES: dict[str, tuple[str, str]] = {
    "bus":        ("[",  "]"),
    "gen":        ("[/", "\\]"),
    "gen_solar":  ("[/", "\\]"),
    "gen_hydro":  ("[/", "\\]"),
    "demand":     ("[\\", "/]"),
    "battery":    ("[(", ")]"),
    "converter":  ("([", "])"),
    "junction":   ("{{", "}}"),
    "reservoir":  ("[/", "/]"),
    "turbine":    ("{",  "}"),
    "flow":       ("[",  "]"),
    "filtration": ("[",  "]"),
}

_MM_STYLES: dict[str, str] = {
    "bus":        "fill:#D6EAF8,stroke:#1A5276,color:#1C2833",
    "gen":        "fill:#FEF9E7,stroke:#E67E22,color:#1C2833",
    "gen_solar":  "fill:#FDEBD0,stroke:#F39C12,color:#1C2833",
    "gen_hydro":  "fill:#D1F2EB,stroke:#1E8449,color:#1C2833",
    "demand":     "fill:#FADBD8,stroke:#C0392B,color:#1C2833",
    "battery":    "fill:#F4ECF7,stroke:#7D3C98,color:#1C2833",
    "converter":  "fill:#D6EAF8,stroke:#1F618D,color:#1C2833",
    "junction":   "fill:#EAFAF1,stroke:#1E8449,color:#1C2833",
    "reservoir":  "fill:#EBF5FB,stroke:#1A5276,color:#1C2833",
    "turbine":    "fill:#D1F2EB,stroke:#1E8449,color:#1C2833",
    "flow":       "fill:#EAF2FF,stroke:#2980B9,color:#1C2833",
    "filtration": "fill:#EAECEE,stroke:#717D7E,color:#1C2833",
}

_MM_ICONS: dict[str, str] = {
    "bus":        "🔌",
    "gen":        "⚡",
    "gen_solar":  "☀️",
    "gen_hydro":  "💧",
    "demand":     "📊",
    "battery":    "🔋",
    "converter":  "🔄",
    "junction":   "🔵",
    "reservoir":  "🏞️",
    "turbine":    "⚙️",
    "flow":       "🌊",
    "filtration": "🔽",
}

_FA_CDN = "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.2/css/all.min.css"

_PYVIS_COLORS: dict[str, dict] = {
    "bus":        {"background": "#D6EAF8", "border": "#1A5276"},
    "gen":        {"background": "#FEF9E7", "border": "#E67E22"},
    "gen_solar":  {"background": "#FDEBD0", "border": "#F39C12"},
    "gen_hydro":  {"background": "#D1F2EB", "border": "#1E8449"},
    "demand":     {"background": "#FADBD8", "border": "#C0392B"},
    "battery":    {"background": "#F4ECF7", "border": "#7D3C98"},
    "converter":  {"background": "#D6EAF8", "border": "#1F618D"},
    "junction":   {"background": "#EAFAF1", "border": "#1E8449"},
    "reservoir":  {"background": "#EBF5FB", "border": "#1A5276"},
    "turbine":    {"background": "#D1F2EB", "border": "#1E8449"},
    "flow":       {"background": "#EAF2FF", "border": "#2980B9"},
    "filtration": {"background": "#EAECEE", "border": "#717D7E"},
}

_PYVIS_SHAPE_MAP: dict[str, str] = {
    "bus":        "square",    "gen":        "triangle",
    "gen_solar":  "triangle",  "gen_hydro":  "triangle",
    "demand":     "triangleDown", "battery": "database",
    "converter":  "ellipse",   "junction":   "hexagon",
    "reservoir":  "box",       "turbine":    "diamond",
    "flow":       "dot",       "filtration": "dot",
}

_PYVIS_SIZE_MAP: dict[str, int] = {
    "bus": 30,  "gen": 24,  "gen_solar": 24,  "gen_hydro": 24,
    "demand": 24, "battery": 30,  "converter": 20,
    "junction": 28, "reservoir": 28, "turbine": 22,
    "flow": 16,  "filtration": 14,
}


# ---------------------------------------------------------------------------
# Graph model
# ---------------------------------------------------------------------------

@dataclass
class Node:
    node_id:  str
    label:    str
    kind:     str
    tooltip:  str = ""
    cluster:  str = ""   # "electrical" or "hydro"


@dataclass
class Edge:
    src:      str
    dst:      str
    label:    str   = ""
    style:    str   = "solid"   # solid | dashed | dotted
    color:    str   = ""
    directed: bool  = True
    weight:   float = 1.0


@dataclass
class GraphModel:
    title:  str              = "gtopt Network"
    nodes:  list[Node]       = field(default_factory=list)
    edges:  list[Edge]       = field(default_factory=list)

    def add_node(self, n: Node) -> None: self.nodes.append(n)
    def add_edge(self, e: Edge) -> None: self.edges.append(e)


# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------

def _scalar(v) -> str:
    if v is None:
        return "\u2014"
    if isinstance(v, (int, float)):
        return str(int(v)) if isinstance(v, float) and v == int(v) else str(v)
    if isinstance(v, list):
        flat: list = []
        def _fl(x):
            if isinstance(x, list):
                for i in x: _fl(i)
            else:
                flat.append(x)
        _fl(v)
        if flat:
            mn, mx = min(flat), max(flat)
            return f"{mn}\u2026{mx}" if mn != mx else _scalar(mn)
        return "\u2014"
    if isinstance(v, str):
        return f'"{v}"'
    return str(v)


def _resolve(arr: list[dict], ref) -> Optional[dict]:
    for item in arr:
        if item.get("uid") == ref or item.get("name") == ref:
            return item
    return None

# ---------------------------------------------------------------------------
# Topology builder — builds a GraphModel from a gtopt JSON file
# ---------------------------------------------------------------------------

class TopologyBuilder:
    def __init__(self, planning: dict, subsystem: str = "full"):
        self.sys      = planning.get("system", {})
        self.subsystem = subsystem
        self.model    = GraphModel(title=self.sys.get("name", "gtopt Network"))

    # ── node ID helpers ──────────────────────────────────────────────────────
    @staticmethod
    def _bid(b):  return f"bus_{b.get('uid', b.get('name','?'))}"
    @staticmethod
    def _gid(g):  return f"gen_{g.get('uid', g.get('name','?'))}"
    @staticmethod
    def _did(d):  return f"dem_{d.get('uid', d.get('name','?'))}"
    @staticmethod
    def _batid(b): return f"bat_{b.get('uid', b.get('name','?'))}"
    @staticmethod
    def _cid(c):  return f"conv_{c.get('uid', c.get('name','?'))}"
    @staticmethod
    def _jid(j):  return f"junc_{j.get('uid', j.get('name','?'))}"
    @staticmethod
    def _rid(r):  return f"res_{r.get('uid', r.get('name','?'))}"
    @staticmethod
    def _tid(t):  return f"turb_{t.get('uid', t.get('name','?'))}"
    @staticmethod
    def _fid(f):  return f"flow_{f.get('uid', f.get('name','?'))}"
    @staticmethod
    def _wwid(w): return f"wway_{w.get('uid', w.get('name','?'))}"

    def _find(self, arr_key: str, ref) -> Optional[dict]:
        return _resolve(self.sys.get(arr_key, []), ref)

    def _find_node_id(self, arr_key: str, ref, id_fn) -> Optional[str]:
        item = self._find(arr_key, ref)
        return id_fn(item) if item else None

    def _gen_kind(self, gen: dict) -> str:
        name  = str(gen.get("name", "")).lower()
        uid   = gen.get("uid") or gen.get("name")
        for t in self.sys.get("turbine_array", []):
            if t.get("generator") in (uid, gen.get("name")):
                return "gen_hydro"
        if any(k in name for k in ("solar", "pv", "foto", "wind", "eol")):
            return "gen_solar"
        return "gen"

    # ── public build entry point ─────────────────────────────────────────────
    def build(self) -> GraphModel:
        s = self.subsystem
        if s in ("full", "electrical"):
            self._buses(); self._generators(); self._demands()
            self._lines(); self._batteries(); self._converters()
        if s in ("full", "hydro"):
            self._junctions(); self._waterways(); self._reservoirs()
            self._turbines();  self._flows();     self._filtrations()
        return self.model

    # ── electrical elements ──────────────────────────────────────────────────
    def _buses(self):
        for bus in self.sys.get("bus_array", []):
            v    = f"\n{bus['voltage']} kV" if "voltage" in bus else ""
            name = bus.get("name", bus.get("uid", "?"))
            self.model.add_node(Node(
                node_id=self._bid(bus), label=f"{name}{v}", kind="bus",
                cluster="electrical",
                tooltip=f"Bus uid={bus.get('uid')} name={name}{v}",
            ))

    def _generators(self):
        for gen in self.sys.get("generator_array", []):
            name  = gen.get("name", gen.get("uid", "?"))
            pmax  = _scalar(gen.get("pmax") or gen.get("capacity"))
            gcost = _scalar(gen.get("gcost", "\u2014"))
            kind  = self._gen_kind(gen)
            self.model.add_node(Node(
                node_id=self._gid(gen), label=f"{name}\n{pmax} MW  {gcost} $/MWh",
                kind=kind, cluster="electrical",
                tooltip=f"Generator uid={gen.get('uid')} pmax={pmax} gcost={gcost}",
            ))
            bus_id = self._find_node_id("bus_array", gen.get("bus"), self._bid)
            if bus_id:
                self.model.add_edge(Edge(self._gid(gen), bus_id,
                                        color=_PALETTE[f"{kind}_border"]))

    def _demands(self):
        for dem in self.sys.get("demand_array", []):
            name = dem.get("name", dem.get("uid", "?"))
            lmax = _scalar(dem.get("lmax"))
            self.model.add_node(Node(
                node_id=self._did(dem), label=f"{name}\n{lmax} MW",
                kind="demand", cluster="electrical",
                tooltip=f"Demand uid={dem.get('uid')} name={name} lmax={lmax}",
            ))
            bus_id = self._find_node_id("bus_array", dem.get("bus"), self._bid)
            if bus_id:
                self.model.add_edge(Edge(bus_id, self._did(dem),
                                        color=_PALETTE["demand_border"]))

    def _lines(self):
        for line in self.sys.get("line_array", []):
            name = line.get("name", line.get("uid", "?"))
            x    = line.get("reactance", "")
            tmax = line.get("tmax_ab", line.get("tmax_ba", ""))
            parts = [name]
            if x:    parts.append(f"x={x} p.u.")
            if tmax: parts.append(f"{_scalar(tmax)} MW")
            bus_a = self._find_node_id("bus_array", line.get("bus_a"), self._bid)
            bus_b = self._find_node_id("bus_array", line.get("bus_b"), self._bid)
            if bus_a and bus_b:
                self.model.add_edge(Edge(
                    src=bus_a, dst=bus_b, label="\n".join(parts),
                    color=_PALETTE["line_edge"], directed=False,
                    weight=float(tmax) if tmax else 1.0,
                ))

    def _batteries(self):
        for bat in self.sys.get("battery_array", []):
            name = bat.get("name", bat.get("uid", "?"))
            emax = _scalar(bat.get("emax") or bat.get("capacity"))
            ein  = bat.get("input_efficiency", "")
            eout = bat.get("output_efficiency", "")
            eff  = f"\n\u03b7={ein}/{eout}" if ein else ""
            self.model.add_node(Node(
                node_id=self._batid(bat), label=f"{name}\n{emax} MWh{eff}",
                kind="battery", cluster="electrical",
                tooltip=f"Battery uid={bat.get('uid')} emax={emax}",
            ))

    def _converters(self):
        for conv in self.sys.get("converter_array", []):
            name = conv.get("name", conv.get("uid", "?"))
            cap  = _scalar(conv.get("capacity"))
            cid  = self._cid(conv)
            self.model.add_node(Node(
                node_id=cid, label=f"{name}\n{cap} MW",
                kind="converter", cluster="electrical",
                tooltip=f"Converter uid={conv.get('uid')} cap={cap}",
            ))
            bat_id = self._find_node_id("battery_array",  conv.get("battery"),   self._batid)
            gen_id = self._find_node_id("generator_array", conv.get("generator"), self._gid)
            dem_id = self._find_node_id("demand_array",    conv.get("demand"),    self._did)
            if bat_id:
                self.model.add_edge(Edge(bat_id, cid, label="stored\nenergy",
                                        style="dashed", color=_PALETTE["bat_link_edge"]))
            if gen_id:
                self.model.add_edge(Edge(cid, gen_id, label="discharge",
                                        style="dashed", color=_PALETTE["bat_link_edge"]))
            if dem_id:
                self.model.add_edge(Edge(dem_id, cid, label="charge",
                                        style="dashed", color=_PALETTE["bat_link_edge"]))

    # ── hydro elements ───────────────────────────────────────────────────────
    def _junctions(self):
        for j in self.sys.get("junction_array", []):
            name = j.get("name", j.get("uid", "?"))
            self.model.add_node(Node(
                node_id=self._jid(j), label=name,
                kind="junction", cluster="hydro",
                tooltip=f"Junction uid={j.get('uid')} name={name}",
            ))

    def _waterways(self):
        for w in self.sys.get("waterway_array", []):
            name = w.get("name", w.get("uid", "?"))
            fmax = _scalar(w.get("fmax"))
            ja   = self._find_node_id("junction_array", w.get("junction_a"), self._jid)
            jb   = self._find_node_id("junction_array", w.get("junction_b"), self._jid)
            if ja and jb:
                self.model.add_edge(Edge(
                    src=ja, dst=jb, label=f"{name}\n\u2264{fmax} m\u00b3/s",
                    color=_PALETTE["waterway_edge"],
                    weight=float(w["fmax"]) if w.get("fmax") else 1.0,
                ))

    def _reservoirs(self):
        for r in self.sys.get("reservoir_array", []):
            name = r.get("name", r.get("uid", "?"))
            emax = _scalar(r.get("emax") or r.get("capacity"))
            self.model.add_node(Node(
                node_id=self._rid(r), label=f"{name}\n{emax} dam\u00b3",
                kind="reservoir", cluster="hydro",
                tooltip=f"Reservoir uid={r.get('uid')} emax={emax}",
            ))
            junc_id = self._find_node_id("junction_array", r.get("junction"), self._jid)
            if junc_id:
                self.model.add_edge(Edge(self._rid(r), junc_id,
                                        style="dashed", color=_PALETTE["reservoir_border"]))

    def _turbines(self):
        for t in self.sys.get("turbine_array", []):
            name = t.get("name", t.get("uid", "?"))
            cap  = _scalar(t.get("capacity"))
            cr   = _scalar(t.get("conversion_rate"))
            tid  = self._tid(t)
            self.model.add_node(Node(
                node_id=tid, label=f"{name}\n{cap} MW  cr={cr}",
                kind="turbine", cluster="hydro",
                tooltip=f"Turbine uid={t.get('uid')} cap={cap}",
            ))
            way = _resolve(self.sys.get("waterway_array", []), t.get("waterway"))
            if way:
                ja = self._find_node_id("junction_array", way.get("junction_a"), self._jid)
                if ja:
                    self.model.add_edge(Edge(ja, tid, label="water in",
                                            color=_PALETTE["waterway_edge"]))
            gen_id = self._find_node_id("generator_array", t.get("generator"), self._gid)
            if gen_id:
                self.model.add_edge(Edge(tid, gen_id, label="power out",
                                        color=_PALETTE["gen_hydro_border"], style="dashed"))

    def _flows(self):
        for f in self.sys.get("flow_array", []):
            name  = f.get("name", f.get("uid", "?"))
            disc  = _scalar(f.get("discharge"))
            direction = f.get("direction", 1)
            fid   = self._fid(f)
            self.model.add_node(Node(
                node_id=fid, label=f"{name}\n{disc} m\u00b3/s",
                kind="flow", cluster="hydro",
                tooltip=f"Flow uid={f.get('uid')} discharge={disc}",
            ))
            junc_id = self._find_node_id("junction_array", f.get("junction"), self._jid)
            if junc_id:
                src, dst = (fid, junc_id) if direction >= 0 else (junc_id, fid)
                self.model.add_edge(Edge(src, dst, color=_PALETTE["waterway_edge"]))

    def _filtrations(self):
        for fi in self.sys.get("filtration_array", []):
            name   = fi.get("name", fi.get("uid", "?"))
            wway   = _resolve(self.sys.get("waterway_array", []), fi.get("waterway"))
            res_id = self._find_node_id("reservoir_array", fi.get("reservoir"), self._rid)
            if wway and res_id:
                ja = self._find_node_id("junction_array", wway.get("junction_a"), self._jid)
                if ja:
                    self.model.add_edge(Edge(ja, res_id,
                        label=f"{name}\n(filtration)",
                        style="dotted", color=_PALETTE["filtration_border"]))

# ---------------------------------------------------------------------------
# Planning structure diagram — pure SVG generator
# ---------------------------------------------------------------------------

_DEFAULT_PLANNING: dict = {
    "system": {"name": "gtopt (example)"},
    "simulation": {
        "scenario_array": [
            {"uid": 1, "name": "Scenario 1 (dry year)",  "probability_factor": 0.4},
            {"uid": 2, "name": "Scenario 2 (normal)",    "probability_factor": 0.4},
            {"uid": 3, "name": "Scenario 3 (wet year)",  "probability_factor": 0.2},
        ],
        "stage_array": [
            {"uid": 1, "name": "Stage 1", "first_block": 0, "count_block": 2, "discount_factor": 1.0},
            {"uid": 2, "name": "Stage 2", "first_block": 2, "count_block": 2, "discount_factor": 0.91},
            {"uid": 3, "name": "Stage 3", "first_block": 4, "count_block": 2, "discount_factor": 0.83},
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
    sim      = planning.get("simulation", _DEFAULT_PLANNING["simulation"])
    blocks   = sim.get("block_array",   _DEFAULT_PLANNING["simulation"]["block_array"])
    stages   = sim.get("stage_array",   _DEFAULT_PLANNING["simulation"]["stage_array"])
    phases   = sim.get("phase_array",   [])
    scenarios= sim.get("scenario_array",_DEFAULT_PLANNING["simulation"]["scenario_array"])
    sys_name = planning.get("system", {}).get("name", "gtopt")

    n_blocks = len(blocks)
    n_stages = len(stages)
    n_phases = len(phases)
    n_scen   = len(scenarios)

    # Layout constants
    M         = 48                         # left/right margin
    LABEL_W   = 120                        # width of left label column
    W         = max(920, LABEL_W + M + n_blocks * 110)
    TITLE_H   = 52
    SEC_GAP   = 10
    SCEN_H    = 28 + n_scen * 32
    PHASE_H   = 48 if n_phases else 0
    STAGE_H   = 72
    BLOCK_H   = 74
    FORMULA_H = 92
    H = TITLE_H + SEC_GAP + SCEN_H + SEC_GAP + PHASE_H + STAGE_H + BLOCK_H + FORMULA_H + M

    CONTENT_W = W - LABEL_W - M
    BLOCK_W   = CONTENT_W / max(n_blocks, 1)

    def bx(i: int) -> float:      # left edge of block i
        return LABEL_W + i * BLOCK_W

    # Colours
    C_BG          = "#FAFBFC"
    C_BORDER      = "#CBD5E0"
    C_TITLE       = "#1A252F"
    C_SCEN_FILL   = "#E8F5E9";  C_SCEN_BDR = "#2E7D32";  C_SCEN_TXT = "#1B5E20"
    C_PHASE_FILL  = "#E3F2FD";  C_PHASE_BDR= "#1565C0";  C_PHASE_TXT= "#0D47A1"
    C_STAGE_FILL  = "#FFF8E1";  C_STAGE_BDR= "#E65100";  C_STAGE_TXT= "#BF360C"
    C_BLOCK_FILL  = "#F3E5F5";  C_BLOCK_BDR= "#6A1B9A";  C_BLOCK_TXT= "#4A148C"
    C_FORM_FILL   = "#ECEFF1";  C_FORM_BDR = "#546E7A";  C_FORM_TXT = "#37474F"
    C_LINESEP     = "#CFD8DC"
    F = "font-family=\'Arial, sans-serif\'"

    def r(x, y, w, h, fill, stroke, rx=6, sw=1.5):
        return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                f'rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')

    def t(x, y, s, size=12, color="#1A252F", anchor="middle", weight="normal", italic=False):
        style = " font-style=\'italic\'" if italic else ""
        return (f'<text x="{x:.1f}" y="{y:.1f}" {F} font-size="{size}" fill="{color}" '
                f'text-anchor="{anchor}" font-weight="{weight}"{style}>{s}</text>')

    def ln(x1, y1, x2, y2, color=C_LINESEP, sw=1, dash=""):
        da = f' stroke-dasharray="{dash}"' if dash else ""
        return (f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                f'stroke="{color}" stroke-width="{sw}"{da}/>')

    el: list[str] = []

    # Canvas
    el.append(f'<rect width="{W}" height="{H}" fill="{C_BG}"/>')
    el.append(r(3, 3, W-6, H-6, "none", C_BORDER, rx=10, sw=2))

    # Title
    el.append(t(W/2, 32, f"{sys_name} \u2014 Planning Time Structure",
                size=17, weight="bold", color=C_TITLE))
    el.append(ln(M, 46, W-M, 46, color=C_LINESEP, sw=1.5))

    y = TITLE_H + SEC_GAP

    # ---- SCENARIOS ---------------------------------------------------------
    lx = LABEL_W - 8
    el.append(t(lx, y+16, "SCENARIOS", size=11, weight="bold", color=C_SCEN_BDR, anchor="end"))
    el.append(t(lx, y+30, "(parallel futures,", size=8, color=C_SCEN_BDR, anchor="end", italic=True))
    el.append(t(lx, y+41, "weighted by prob)", size=8, color=C_SCEN_BDR, anchor="end", italic=True))
    bw = W - LABEL_W - M
    for si, sc in enumerate(scenarios[:8]):
        sy = y + 8 + si * 32
        prob = sc.get("probability_factor", "")
        sc_name = sc.get("name") or f"Scenario {sc.get('uid', si+1)}"
        el.append(r(LABEL_W, sy, bw, 26, C_SCEN_FILL, C_SCEN_BDR, rx=5, sw=1.5))
        lbl = f"{sc_name}  \u2014  probability_factor = {prob}"
        el.append(t(LABEL_W + bw/2, sy+17, lbl, size=11, color=C_SCEN_TXT))
    if n_scen > 8:
        sy = y + 8 + 8*32
        el.append(t(LABEL_W + bw/2, sy+10, f"\u2026 and {n_scen-8} more scenarios",
                    size=10, color=C_SCEN_BDR, italic=True))
    y += SCEN_H + SEC_GAP

    # ---- PHASES (optional) -------------------------------------------------
    if n_phases:
        el.append(t(lx, y+18, "PHASES", size=11, weight="bold", color=C_PHASE_BDR, anchor="end"))
        el.append(t(lx, y+30, "(group stages)", size=8, color=C_PHASE_BDR, anchor="end", italic=True))
        for ph in phases:
            fs  = ph.get("first_stage", 0)
            cs  = ph.get("count_stage", n_stages)
            if fs >= n_stages:
                continue
            last_s = min(fs + cs - 1, n_stages - 1)
            st0    = stages[fs]
            stL    = stages[last_s]
            fb0    = st0.get("first_block", 0)
            fbL    = stL.get("first_block", last_s)
            cbL    = stL.get("count_block", 1)
            px     = bx(fb0) + 1
            pw     = bx(fbL + cbL) - px - 1
            ph_name= ph.get("name") or f"Phase {ph.get('uid','')}"
            el.append(r(px, y+4, pw, PHASE_H-10, C_PHASE_FILL, C_PHASE_BDR, rx=5, sw=1.5))
            el.append(t(px + pw/2, y+24, ph_name, size=11, color=C_PHASE_TXT, weight="bold"))
        y += PHASE_H + SEC_GAP

    # ---- STAGES ------------------------------------------------------------
    el.append(t(lx, y+22, "STAGES", size=11, weight="bold", color=C_STAGE_BDR, anchor="end"))
    el.append(t(lx, y+34, "(investment period,", size=8, color=C_STAGE_BDR, anchor="end", italic=True))
    el.append(t(lx, y+45, "discounted cost)", size=8, color=C_STAGE_BDR, anchor="end", italic=True))
    for st in stages:
        fb = st.get("first_block", 0)
        cb = st.get("count_block", 1)
        if fb >= n_blocks:
            continue
        sx  = bx(fb) + 1
        sw2 = bx(min(fb + cb, n_blocks)) - sx - 1
        df  = st.get("discount_factor", "")
        st_name = st.get("name") or f"Stage {st.get('uid','')}"
        el.append(r(sx, y+4, sw2, STAGE_H-10, C_STAGE_FILL, C_STAGE_BDR, rx=5, sw=1.5))
        el.append(t(sx + sw2/2, y+26, st_name, size=10, color=C_STAGE_TXT, weight="bold"))
        info = f"discount={df}" if df != "" else f"{cb} block(s)"
        el.append(t(sx + sw2/2, y+42, info, size=9, color=C_STAGE_BDR, italic=(df == "")))
    y += STAGE_H

    # ---- BLOCKS ------------------------------------------------------------
    el.append(t(lx, y+20, "BLOCKS", size=11, weight="bold", color=C_BLOCK_BDR, anchor="end"))
    el.append(t(lx, y+33, "(time steps,", size=8, color=C_BLOCK_BDR, anchor="end", italic=True))
    el.append(t(lx, y+44, "energy=power\u00d7dur.)", size=8, color=C_BLOCK_BDR, anchor="end", italic=True))
    for bi, blk in enumerate(blocks):
        bxi  = bx(bi) + 1
        bwi  = BLOCK_W - 2
        dur  = blk.get("duration", "")
        b_name = blk.get("name") or f"B{blk.get('uid', bi+1)}"
        el.append(r(bxi, y+4, bwi, BLOCK_H-10, C_BLOCK_FILL, C_BLOCK_BDR, rx=4, sw=1.5))
        fsize = max(8, min(11, int(bwi / 7)))
        el.append(t(bxi + bwi/2, y+26, b_name, size=fsize, color=C_BLOCK_TXT, weight="bold"))
        if dur != "":
            d_str = f"{dur}h" if float(dur) < 1000 else f"{float(dur)/8760:.2f}yr"
            el.append(t(bxi + bwi/2, y+44, d_str, size=max(7, fsize-1), color=C_BLOCK_BDR))
    y += BLOCK_H

    # ---- OBJECTIVE FUNCTION ------------------------------------------------
    el.append(r(LABEL_W, y+8, W-LABEL_W-M, FORMULA_H-16, C_FORM_FILL, C_FORM_BDR, rx=8, sw=1.5))
    fy = y + 28
    el.append(t(LABEL_W+14, fy, "OBJECTIVE FUNCTION:", size=11, weight="bold",
                color=C_TITLE, anchor="start"))
    fy += 20
    el.append(t(LABEL_W+18, fy,
        "min  \u03a3_s  \u03a3_t  \u03a3_b   prob_s \u00d7 discount_t \u00d7 duration_b \u00d7 dispatch_cost(s, t, b)",
        size=12, color=C_FORM_TXT, anchor="start"))
    fy += 18
    el.append(t(LABEL_W+18, fy,
        "+  \u03a3_t   annual_capcost_t \u00d7 expansion_modules_t      [CAPEX / investment]",
        size=12, color=C_FORM_TXT, anchor="start"))
    fy += 16
    el.append(t(LABEL_W+18, fy,
        "subject to: bus power balance, Kirchhoff voltage law, generator / line / battery / hydro constraints",
        size=10, color="#78909C", anchor="start", italic=True))

    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
            f'width="{W}" height="{H}">\n' + "\n".join(el) + "\n</svg>")


def _build_planning_mermaid(planning: dict) -> str:  # noqa: ARG001
    """Return a Mermaid classDiagram of the gtopt planning data model.

    The *planning* argument is accepted for API consistency but is not used
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
        "    Simulation *-- Phase      : has many (optional)",
        "    Simulation *-- Block      : has many",
        "    Phase      o-- Stage      : groups",
        "    Stage      o-- Block      : references by first_block+count_block",
        "    Scene      --> Scenario   : cross-product",
        "    Scene      --> Phase      : cross-product",
        "```",
    ]
    return "\n".join(lines)


def _build_planning_html(planning: dict, title: str = "gtopt Planning Structure") -> str:
    """Self-contained HTML with the planning SVG and Mermaid class diagram."""
    svg_content  = _build_planning_svg(planning)
    mmd_inner    = "\n".join(_build_planning_mermaid(planning).splitlines()[4:-1])

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
# Graphviz topology renderer
# ---------------------------------------------------------------------------

def _gv_node_label(node: Node, icon_path: Optional[str]) -> str:
    bg     = _PALETTE.get(node.kind, "#FFF")
    border = _PALETTE.get(f"{node.kind}_border", "#333")
    lbl    = (node.label
              .replace("&", "&amp;")
              .replace("<", "&lt;")
              .replace(">", "&gt;")
              .replace("\n", "<BR/>"))

    if icon_path:
        return (
            f'<<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="4"'
            f' BGCOLOR="{bg}" COLOR="{border}">'
            f'<TR><TD ALIGN="CENTER"><IMG SRC="{icon_path}"/></TD></TR>'
            f'<TR><TD ALIGN="CENTER">'
            f'<FONT FACE="Arial" POINT-SIZE="10">{lbl}</FONT>'
            f'</TD></TR></TABLE>>'
        )
    return (
        f'<<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="6"'
        f' BGCOLOR="{bg}" COLOR="{border}">'
        f'<TR><TD ALIGN="CENTER">'
        f'<FONT FACE="Arial" POINT-SIZE="10"><B>{lbl}</B></FONT>'
        f'</TD></TR></TABLE>>'
    )


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
    dot = cls(name=model.title, engine=layout,
              format=fmt if fmt != "dot" else "svg")
    dot.attr(
        label=f"<<B>{model.title}</B>>", labelloc="t",
        fontname="Arial", fontsize="14",
        bgcolor="white", pad="0.5",
        splines="curved", overlap="false",
        nodesep="0.6", ranksep="1.0",
    )
    dot.attr("node", margin="0.05,0.05", shape="none")
    dot.attr("edge", fontname="Arial", fontsize="9", penwidth="1.5")

    elec_nodes  = {n.node_id for n in model.nodes if n.cluster == "electrical"}
    hydro_nodes = {n.node_id for n in model.nodes if n.cluster == "hydro"}
    has_both    = bool(elec_nodes) and bool(hydro_nodes)

    def add_node(d, node: Node) -> None:
        icon = _icon_png_path(node.kind)
        d.node(node.node_id, label=_gv_node_label(node, icon),
               tooltip=node.tooltip or node.label)

    if has_both and use_clusters:
        with dot.subgraph(name="cluster_elec") as c:
            c.attr(label="Electrical Network", bgcolor="#F8FBFF",
                   color="#AED6F1", style="rounded", fontname="Arial Bold")
            for n in model.nodes:
                if n.cluster == "electrical":
                    add_node(c, n)
        with dot.subgraph(name="cluster_hydro") as c:
            c.attr(label="Hydro Cascade", bgcolor="#F0FFF4",
                   color="#A9DFBF", style="rounded", fontname="Arial Bold")
            for n in model.nodes:
                if n.cluster == "hydro":
                    add_node(c, n)
    else:
        for n in model.nodes:
            add_node(dot, n)

    for e in model.edges:
        attrs: dict = {}
        if e.color: attrs["color"] = e.color
        if e.label: attrs["label"] = e.label.replace("\n", "\\n")
        if e.style == "dashed":  attrs.update(style="dashed", penwidth="1.2")
        elif e.style == "dotted": attrs.update(style="dotted", penwidth="1.0")
        if not e.directed:
            attrs["dir"]      = "none"
            attrs["penwidth"] = str(max(1.5, min(4.0, 1.0 + e.weight / 100)))
        dot.edge(e.src, e.dst, **attrs)

    if fmt == "dot":
        return dot.source
    if output_path:
        out = Path(output_path)
        rendered = dot.render(
            filename=str(out.stem),
            directory=str(out.parent) if str(out.parent) != "." else ".",
            format=fmt, cleanup=True,
        )
        return rendered
    return dot.pipe(format=fmt).decode("utf-8", errors="replace")


# ---------------------------------------------------------------------------
# Mermaid renderer
# ---------------------------------------------------------------------------

def render_mermaid(model: GraphModel, direction: str = "LR") -> str:
    lines = ["```mermaid", "---", f"title: {model.title}", "---",
             f"flowchart {direction}", ""]

    for n in model.nodes:
        shapes = _MM_SHAPES.get(n.kind, ("[", "]"))
        icon   = _MM_ICONS.get(n.kind, "")
        safe   = n.node_id.replace("-", "_")
        lbl    = (icon + " " + n.label).replace('"', "'").replace("\n", "<br/>")
        lines.append(f'    {safe}{shapes[0]}"{lbl}"{shapes[1]}')

    lines.append("")
    for e in model.edges:
        s    = e.src.replace("-", "_")
        d    = e.dst.replace("-", "_")
        lbl  = e.label.replace('"', "'").replace("\n", " ") if e.label else ""
        if not e.directed:   arrow = "---"
        elif e.style == "dashed":  arrow = "-.->"
        elif e.style == "dotted":  arrow = "...>"
        else:                arrow = "-->"
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
# Interactive HTML renderer (pyvis + font-awesome + custom icons)
# ---------------------------------------------------------------------------

def _legend_html(model: GraphModel) -> str:
    kinds = {n.kind for n in model.nodes}
    LABELS = {
        "bus":        "Bus (electrical node)",
        "gen":        "Generator (thermal)",
        "gen_solar":  "Generator (solar/wind)",
        "gen_hydro":  "Generator (hydro-linked)",
        "demand":     "Demand / load",
        "battery":    "Battery storage",
        "converter":  "Power converter",
        "junction":   "Hydraulic junction",
        "reservoir":  "Reservoir / dam",
        "turbine":    "Hydraulic turbine",
        "flow":       "Inflow / outflow",
        "filtration": "Filtration / seepage",
    }
    entries = []
    for kind, lbl in LABELS.items():
        if kind not in kinds:
            continue
        icon = _MM_ICONS.get(kind, "\u25cf")
        bg   = _PALETTE.get(kind, "#EEE")
        bd   = _PALETTE.get(f"{kind}_border", "#999")
        entries.append(
            f'<div style="display:flex;align-items:center;gap:8px;margin:3px 0">'
            f'<span style="width:16px;height:16px;background:{bg};border:2px solid {bd};'
            f'border-radius:3px;display:inline-block;text-align:center;'
            f'line-height:14px;font-size:11px">{icon}</span>'
            f'<span>{lbl}</span></div>'
        )
    return (
        '<div style="position:fixed;bottom:20px;right:20px;'
        'background:rgba(255,255,255,0.97);border:1px solid #CCC;border-radius:10px;'
        'padding:14px 18px;font-family:Arial;font-size:12px;'
        'box-shadow:3px 3px 10px rgba(0,0,0,0.12);z-index:9999;min-width:200px;">'
        '<b style="display:block;margin-bottom:8px;font-size:13px">Legend</b>'
        + "".join(entries) + "</div>"
    )


def render_html(model: GraphModel, output_path: str) -> str:
    try:
        from pyvis.network import Network  # noqa: PLC0415
    except ImportError as err:
        raise SystemExit("pip install pyvis") from err

    net = Network(height="750px", width="100%", directed=True,
                  notebook=False, bgcolor="#FAFAFA", font_color="#1C2833")
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
        color = dict(_PYVIS_COLORS.get(node.kind, {"background": "#FFF", "border": "#333"}))
        color["highlight"] = {"background": color["background"], "border": "#E74C3C"}
        size  = _PYVIS_SIZE_MAP.get(node.kind, 20)

        if icon_path:
            net.add_node(
                node.node_id, label=node.label,
                title=node.tooltip or node.label,
                shape="image", image=f"file://{icon_path}",
                size=size + 8, font={"size": 11, "face": "Arial"},
                color=color,
            )
        else:
            net.add_node(
                node.node_id, label=node.label,
                title=node.tooltip or node.label,
                shape=_PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                color=color, size=size,
                font={"size": 11, "face": "Arial"},
            )

    for e in model.edges:
        dashes = e.style in ("dashed", "dotted")
        c      = e.color or "#2C3E50"
        net.add_edge(
            e.src, e.dst,
            label=e.label.replace("\n", " ") if e.label else "",
            title=e.label.replace("\n", " ") if e.label else "",
            color={"color": c, "opacity": 0.80},
            dashes=dashes,
            arrows="to" if e.directed else "",
            width=max(1.0, min(4.0, 1.0 + e.weight / 100)) if not dashes else 1.0,
        )

    net.save_graph(output_path)
    html  = Path(output_path).read_text(encoding="utf-8")
    fa    = f'<link rel="stylesheet" href="{_FA_CDN}">\n'
    info  = (
        f'<div style="position:fixed;top:0;left:0;right:0;background:#1A252F;'
        f'color:white;padding:8px 20px;font-family:Arial;font-size:13px;'
        f'z-index:10000;display:flex;align-items:center;gap:20px;">'
        f'<b>gtopt</b> \u2014 <span>{model.title}</span>'
        f'<span style="margin-left:auto;font-size:11px;opacity:0.7">'
        f'{len(model.nodes)} elements \u00b7 {len(model.edges)} connections</span>'
        f'</div><div style="margin-top:40px"></div>'
    )
    html  = html.replace("</head>", fa + "</head>", 1)
    html  = html.replace("<body>",  f"<body>\n{info}", 1)
    html  = html.replace("</body>", f"\n{_legend_html(model)}\n</body>", 1)
    Path(output_path).write_text(html, encoding="utf-8")
    return output_path


# ---------------------------------------------------------------------------
# Auto layout selection
# ---------------------------------------------------------------------------

def _auto_layout(model: GraphModel, subsystem: str) -> str:
    n = len(model.nodes)
    if subsystem == "hydro":
        return "dot"
    if n <= 8:
        return "dot"
    if n <= 25:
        return "neato"
    return "fdp"


# ---------------------------------------------------------------------------
# Main CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:  # noqa: C901
    parser = argparse.ArgumentParser(
        description=(
            "Generate electrical, hydro, and planning-structure diagrams "
            "from a gtopt JSON planning file."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Diagram types:
              topology   Network topology (buses, generators, lines, hydro elements, ...)
              planning   Conceptual time-structure (blocks / stages / phases / scenarios)

            Subsystems (topology only):
              full        All elements together (default)
              electrical  Buses, generators, demands, lines, batteries, converters
              hydro       Junctions, waterways, reservoirs, turbines, flows, filtrations

            Output formats:
              svg, png, pdf, dot   Graphviz-rendered static images
              mermaid              Mermaid source for GitHub Markdown
              html                 Interactive vis.js browser diagram

            Examples:
              python3 scripts/gtopt_diagram.py cases/ieee_9b/ieee_9b.json -o ieee9b.svg
              python3 scripts/gtopt_diagram.py cases/bat_4b/bat_4b.json \\
                  --subsystem electrical --format html -o bat4b.html
              python3 scripts/gtopt_diagram.py cases/bat_4b/bat_4b.json \\
                  --subsystem hydro --format svg -o hydro.svg
              python3 scripts/gtopt_diagram.py --diagram-type planning -o planning.svg
              python3 scripts/gtopt_diagram.py cases/c0/system_c0.json \\
                  --diagram-type planning --format html
              python3 scripts/gtopt_diagram.py cases/bat_4b/bat_4b.json --format mermaid
        """),
    )
    parser.add_argument("json_file", nargs="?",
                        help="gtopt JSON planning file (optional for planning diagrams)")
    parser.add_argument("--diagram-type", "-t",
                        choices=["topology", "planning"], default="topology",
                        help="Diagram type (default: topology)")
    parser.add_argument("--format", "-f",
                        choices=["dot", "png", "svg", "pdf", "mermaid", "html"],
                        default="svg",
                        help="Output format (default: svg)")
    parser.add_argument("--output", "-o", default=None,
                        help="Output file path")
    parser.add_argument("--subsystem", "-s",
                        choices=["full", "electrical", "hydro"],
                        default="full",
                        help="Subsystem for topology diagrams (default: full)")
    parser.add_argument("--layout", "-l",
                        choices=["dot", "neato", "fdp", "sfdp", "circo", "twopi"],
                        default=None,
                        help="Graphviz layout engine (auto-selected if omitted)")
    parser.add_argument("--direction", "-d",
                        choices=["LR", "TD", "BT", "RL"],
                        default="LR",
                        help="Mermaid flowchart direction (default: LR)")
    parser.add_argument("--clusters",
                        action="store_true", default=False,
                        help="Group electrical/hydro in Graphviz clusters (full topology)")
    args = parser.parse_args(argv)

    # Load JSON if provided
    planning: dict = {}
    case_name = "gtopt"
    if args.json_file:
        jp = Path(args.json_file)
        if not jp.exists():
            print(f"Error: file not found: {jp}", file=sys.stderr)
            return 1
        with jp.open(encoding="utf-8") as fh:
            planning = json.load(fh)
        case_name = planning.get("system", {}).get("name", jp.stem)

    fmt = args.format
    sfx = {"dot": ".dot", "png": ".png", "svg": ".svg", "pdf": ".pdf",
           "mermaid": ".md", "html": ".html"}.get(fmt, f".{fmt}")
    out = args.output or f"{case_name}_{args.diagram_type}{sfx}"

    # ── Planning structure diagram ──────────────────────────────────────────
    if args.diagram_type == "planning":
        if fmt == "mermaid":
            result = _build_planning_mermaid(planning)
            if args.output:
                Path(out).write_text(result, encoding="utf-8")
                print(f"Mermaid planning diagram written to {out}", file=sys.stderr)
            else:
                print(result)
            return 0
        if fmt == "html":
            ttl = (f"{case_name} \u2014 Planning Structure"
                   if case_name != "gtopt" else "gtopt Planning Structure")
            Path(out).write_text(_build_planning_html(planning, title=ttl),
                                 encoding="utf-8")
            print(f"Planning HTML written to {out}", file=sys.stderr)
            return 0
        # SVG / PNG / PDF / DOT
        svg_src = _build_planning_svg(planning)
        if fmt in ("svg", "dot"):
            Path(out).write_text(svg_src, encoding="utf-8")
        else:
            try:
                import cairosvg  # noqa: PLC0415
                if fmt == "png":
                    cairosvg.svg2png(bytestring=svg_src.encode(), write_to=out, scale=2.0)
                elif fmt == "pdf":
                    cairosvg.svg2pdf(bytestring=svg_src.encode(), write_to=out)
                else:
                    Path(out).write_text(svg_src, encoding="utf-8")
            except ImportError:
                print("Warning: cairosvg not found; writing SVG instead", file=sys.stderr)
                out = out.rsplit(".", 1)[0] + ".svg"
                Path(out).write_text(svg_src, encoding="utf-8")
        print(f"Planning diagram written to {out}", file=sys.stderr)
        return 0

    # ── Topology diagram ────────────────────────────────────────────────────
    builder = TopologyBuilder(planning, subsystem=args.subsystem)
    model   = builder.build()

    if not model.nodes:
        print("Warning: no elements found for the requested subsystem.", file=sys.stderr)

    if fmt == "mermaid":
        result = render_mermaid(model, direction=args.direction)
        if args.output:
            Path(out).write_text(result, encoding="utf-8")
            print(f"Mermaid topology written to {out}", file=sys.stderr)
        else:
            print(result)
        return 0

    if fmt == "html":
        render_html(model, output_path=out)
        print(f"Interactive HTML written to {out}", file=sys.stderr)
        return 0

    layout   = args.layout or _auto_layout(model, args.subsystem)
    clusters = args.clusters and args.subsystem == "full"

    if fmt == "dot":
        src = render_graphviz(model, fmt="dot", layout=layout, use_clusters=clusters)
        if args.output:
            Path(out).write_text(src, encoding="utf-8")
            print(f"DOT source written to {out}", file=sys.stderr)
        else:
            print(src)
        return 0

    rendered = render_graphviz(model, fmt=fmt, output_path=out,
                                layout=layout, use_clusters=clusters)
    print(f"Diagram written to {rendered}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

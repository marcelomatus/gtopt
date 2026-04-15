# SPDX-License-Identifier: BSD-3-Clause
"""SVG icon definitions, colour palettes, shape maps, and layout constants.

This module consolidates all static visual configuration used by the diagram
generators: icon SVG strings, palettes (default and colorblind), Mermaid shape
and style mappings, Graphviz shape mappings, pyvis configuration, and layout
algorithm thresholds.
"""

from __future__ import annotations

import base64
import hashlib
import os
import tempfile
from typing import Optional

from gtopt_diagram._classify import GEN_TYPE_META as _GEN_TYPE_META  # noqa: F401

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

_ICON_SVG["seepage"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#E8E8E8"/><stop offset="100%" stop-color="#95A5A6"/>
  </linearGradient></defs>
  <path d="M3 7 L45 7 L29 27 L29 43 L19 43 L19 27 Z"
        fill="url(#g)" stroke="#717D7E" stroke-width="2.5" stroke-linejoin="round"/>
  <line x1="11" y1="14" x2="37" y2="14" stroke="#7F8C8D" stroke-width="1.5" stroke-dasharray="3,2"/>
  <line x1="16" y1="21" x2="32" y2="21" stroke="#7F8C8D" stroke-width="1.5" stroke-dasharray="3,2"/>
  <ellipse cx="24" cy="47" rx="2.5" ry="3.5" fill="#3498DB" opacity="0.85"/>
</svg>"""

_ICON_SVG[
    "volume_right"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#D7CCC8"/><stop offset="100%" stop-color="#795548"/>
  </linearGradient></defs>
  <polygon points="4,44 11,10 37,10 44,44" fill="#BCAAA4" stroke="#6D4C41" stroke-width="2"/>
  <path d="M13 14 L35 14 L40 44 L8 44 Z" fill="url(#g)" opacity="0.85"/>
  <path d="M8 30 Q16 26 24 30 Q32 34 40 30" fill="none" stroke="#A1887F" stroke-width="1.5"/>
  <text x="24" y="43" text-anchor="middle" font-family="Arial" font-size="9"
        font-weight="bold" fill="#FFF8E1">V</text>
  <path d="M22 5 L26 5 L24 9 Z" fill="#F57F17"/>
  <circle cx="24" cy="4" r="3" fill="none" stroke="#F57F17" stroke-width="1.5"/>
</svg>"""

_ICON_SVG[
    "flow_right"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="0%">
    <stop offset="0%" stop-color="#B2EBF2"/><stop offset="100%" stop-color="#00838F"/>
  </linearGradient></defs>
  <path d="M3 20 Q12 14 21 20 Q30 26 39 20 L43 23 L39 29 Q30 23 21 29 Q12 35 3 29 Z"
        fill="url(#g)" stroke="#00695C" stroke-width="1.5"/>
  <path d="M22 5 L26 5 L24 9 Z" fill="#F57F17"/>
  <circle cx="24" cy="4" r="3" fill="none" stroke="#F57F17" stroke-width="1.5"/>
  <text x="24" y="44" text-anchor="middle" font-family="Arial" font-size="9"
        font-weight="bold" fill="#006064">Q</text>
</svg>"""

# Pump: centrifugal pump symbol — impeller circle with inlet/outlet arrows
_ICON_SVG["pump"] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="100%" y2="100%">
    <stop offset="0%" stop-color="#E3F2FD"/><stop offset="100%" stop-color="#1565C0"/>
  </linearGradient></defs>
  <!-- Pump casing -->
  <circle cx="24" cy="28" r="16" fill="url(#g)" stroke="#0D47A1" stroke-width="2.5"/>
  <!-- Impeller blades -->
  <path d="M24 20 Q29 22 30 28 Q25 26 24 20" fill="#1565C0" opacity="0.85"/>
  <path d="M30 28 Q30 34 24 36 Q24 30 30 28" fill="#1565C0" opacity="0.85"/>
  <path d="M24 36 Q18 34 18 28 Q23 30 24 36" fill="#1565C0" opacity="0.85"/>
  <path d="M18 28 Q19 22 24 20 Q23 26 18 28" fill="#1565C0" opacity="0.85"/>
  <circle cx="24" cy="28" r="4" fill="#0D47A1"/>
  <!-- Inlet: water arrow from top (upstream push) -->
  <line x1="24" y1="4" x2="24" y2="12" stroke="#0277BD" stroke-width="2.5" stroke-linecap="round"/>
  <polygon points="24,12 20,7 28,7" fill="#0277BD"/>
  <!-- Outlet: power bolt from left (electrical demand) -->
  <line x1="4" y1="28" x2="12" y2="28" stroke="#E67E22" stroke-width="2.5" stroke-linecap="round" stroke-dasharray="3,2"/>
  <polygon points="4,28 9,24 9,32" fill="#E67E22"/>
</svg>"""

# LNG Terminal: cylindrical storage tank with flame
_ICON_SVG[
    "lng_terminal"
] = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <defs><linearGradient id="g" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#E3F2FD"/><stop offset="100%" stop-color="#0D47A1"/>
  </linearGradient>
  <linearGradient id="flame" x1="0%" y1="0%" x2="0%" y2="100%">
    <stop offset="0%" stop-color="#FFF176"/><stop offset="100%" stop-color="#FF6F00"/>
  </linearGradient></defs>
  <!-- Tank cylinder body -->
  <rect x="8" y="16" width="32" height="26" rx="3" fill="url(#g)" stroke="#01579B" stroke-width="2.5"/>
  <!-- Tank dome top -->
  <ellipse cx="24" cy="16" rx="16" ry="5" fill="#1976D2" stroke="#01579B" stroke-width="2"/>
  <!-- LNG wave inside -->
  <path d="M10 30 Q14 26 18 30 Q22 34 26 30 Q30 26 38 30"
        fill="none" stroke="#90CAF9" stroke-width="1.5" opacity="0.8"/>
  <!-- Flame / gas symbol -->
  <path d="M22 8 Q20 4 24 2 Q28 4 26 8 Q28 6 29 8 Q27 12 24 13 Q21 12 19 8 Q20 6 22 8"
        fill="url(#flame)" stroke="#E65100" stroke-width="1"/>
  <!-- LNG text -->
  <text x="24" y="38" text-anchor="middle" font-family="Arial" font-size="7"
        font-weight="bold" fill="#E3F2FD">LNG</text>
  <!-- Outlet pipe at bottom -->
  <line x1="24" y1="42" x2="24" y2="47" stroke="#01579B" stroke-width="2" stroke-linecap="round"/>
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


# ---------------------------------------------------------------------------
# Generator-type icon SVGs
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


# ---------------------------------------------------------------------------
# Icon cache and base64 utilities
# ---------------------------------------------------------------------------


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
    # Determine the SVG source — prefer element icons, fall back to base kind,
    # then gen-type icons
    svg_src = _ICON_SVG.get(kind)
    if svg_src is None and "_" in kind:
        svg_src = _ICON_SVG.get(kind.rsplit("_", 1)[0])
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
    """Return a base64 data URI for the best SVG icon for *kind*.

    Falls back to the base kind (e.g. reservoir_sm -> reservoir)
    and then to generator-type icons.
    """
    svg = _ICON_SVG.get(kind)
    # Fallback: strip intensity/variant suffix (e.g. reservoir_lg -> reservoir)
    if svg is None and "_" in kind:
        base = kind.rsplit("_", 1)[0]
        svg = _ICON_SVG.get(base)
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
    # Electrical elements — green tones
    "bus": "#E8F5E9",
    "bus_border": "#2E7D32",
    "gen": "#FEF9E7",
    "gen_border": "#E67E22",
    "gen_solar": "#FDEBD0",
    "gen_solar_border": "#F39C12",
    "gen_hydro": "#E3F2FD",
    "gen_hydro_border": "#1565C0",
    "demand": "#FADBD8",
    "demand_border": "#C0392B",
    "battery": "#F4ECF7",
    "battery_border": "#7D3C98",
    "converter": "#E8F5E9",
    "converter_border": "#388E3C",
    # Hydro elements — blue tones
    "junction": "#E3F2FD",
    "junction_border": "#1565C0",
    "reservoir": "#E1F5FE",
    "reservoir_border": "#01579B",
    "turbine": "#BBDEFB",
    "turbine_border": "#1565C0",
    "flow": "#E3F2FD",
    "flow_border": "#0277BD",
    "seepage": "#E0E0E0",
    "seepage_border": "#616161",
    # Water rights — brownish tones for volume, teal for flow
    "volume_right": "#EFEBE9",
    "volume_right_border": "#6D4C41",
    "flow_right": "#E0F7FA",
    "flow_right_border": "#00838F",
    "right_edge": "#F57F17",
    "right_bound_edge": "#8D6E63",
    # Pump — dark blue (hydro element consuming electricity)
    "pump": "#E3F2FD",
    "pump_border": "#0D47A1",
    "pump_edge": "#1565C0",
    # LNG terminal — deep blue with orange gas accent
    "lng_terminal": "#E8EAF6",
    "lng_terminal_border": "#01579B",
    "lng_edge": "#FF6F00",
    "line_edge": "#4CAF50",
    "power_edge": "#66BB6A",
    "waterway_edge": "#0277BD",
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
    "seepage": "#ECEFF1",
    "seepage_border": "#546E7A",
    "reserve_zone": "#FFF8E1",
    "reserve_zone_border": "#F57F17",
    "gen_profile": "#F3E5F5",
    "gen_profile_border": "#7B1FA2",
    "dem_profile": "#FCE4EC",
    "dem_profile_border": "#AD1457",
    # Water rights — brownish tones for volume, teal for flow
    "volume_right": "#EFEBE9",
    "volume_right_border": "#4E342E",
    "flow_right": "#E0F7FA",
    "flow_right_border": "#006064",
    "right_edge": "#E65100",
    "right_bound_edge": "#5D4037",
    # Pump
    "pump": "#E1F5FE",
    "pump_border": "#01579B",
    "pump_edge": "#0277BD",
    # LNG terminal
    "lng_terminal": "#E8EAF6",
    "lng_terminal_border": "#1A237E",
    "lng_edge": "#E65100",
    "line_edge": "#4CAF50",
    "power_edge": "#66BB6A",
    "waterway_edge": "#0277BD",
    "bat_link_edge": "#4A148C",
    "efficiency_edge": "#BF360C",
    "reserve_edge": "#F57F17",
    "profile_edge": "#95A5A6",
}

# Voltage-based line coloring: maps voltage ranges to (color, width).
# Higher voltages get warmer/darker colors and wider lines.
# Scale: green (low kV) -> orange/brown (high kV). Never blue.
_LINE_VOLTAGE_BANDS: list[tuple[float, str, float]] = [
    # (min_kv, color, width)
    (500.0, "#BF360C", 5.0),  # 500+ kV: dark orange-red, extra wide
    (345.0, "#E65100", 4.5),  # 345 kV: deep orange
    (220.0, "#F57F17", 4.0),  # 220 kV: amber
    (154.0, "#F9A825", 3.5),  # 154 kV: yellow-amber
    (110.0, "#9E9D24", 3.0),  # 110 kV: olive
    (66.0, "#689F38", 2.5),  # 66 kV: light green
    (33.0, "#7CB342", 2.0),  # 33 kV: green
    (0.0, "#AED581", 1.5),  # unknown/LV: pale green
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
    "seepage": "ReservoirSeepage",
    "pump": "Pump",
    "volume_right": "VolumeRight",
    "flow_right": "FlowRight",
    "lng_terminal": "LNG Terminal",
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
    "reservoir_sm": ("[/", "/]"),
    "reservoir_md": ("[/", "/]"),
    "reservoir_lg": ("[/", "/]"),
    "turbine": ("{", "}"),
    "flow": ("((", "))"),
    "seepage": ("((", "))"),
    "pump": ("{", "}"),
    "volume_right": ("[\\", "/]"),
    "flow_right": ("((", "))"),
    "lng_terminal": ("([", "])"),
    "reserve_zone": (">", "]"),
    "gen_profile": ("[", "]"),
    "dem_profile": ("[", "]"),
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
    "reservoir_sm": "fill:#E1F5FE,stroke:#4FC3F7,color:#1C2833",
    "reservoir_md": "fill:#81D4FA,stroke:#0288D1,color:#1C2833",
    "reservoir_lg": "fill:#039BE5,stroke:#01579B,color:#FFF",
    "turbine": "fill:#D1F2EB,stroke:#1E8449,color:#1C2833",
    "flow": "fill:#EAF2FF,stroke:#2980B9,color:#1C2833",
    "seepage": "fill:#EAECEE,stroke:#717D7E,color:#1C2833",
    "pump": "fill:#E3F2FD,stroke:#0D47A1,color:#1C2833",
    "volume_right": "fill:#EFEBE9,stroke:#6D4C41,color:#1C2833",
    "flow_right": "fill:#E0F7FA,stroke:#00838F,color:#1C2833",
    "lng_terminal": "fill:#E8EAF6,stroke:#01579B,color:#1C2833",
    "reserve_zone": "fill:#FFF8E1,stroke:#F57F17,color:#1C2833",
    "gen_profile": "fill:#F3E5F5,stroke:#7B1FA2,color:#1C2833",
    "dem_profile": "fill:#FCE4EC,stroke:#AD1457,color:#1C2833",
}

_MM_ICONS: dict[str, str] = {
    "bus": "\U0001f50c",
    "gen": "\u26a1",
    "gen_solar": "\u2600\ufe0f",
    "gen_hydro": "\U0001f4a7",
    "demand": "\U0001f4ca",
    "battery": "\U0001f50b",
    "converter": "\U0001f504",
    "junction": "\U0001f535",
    "reservoir": "\U0001f3de\ufe0f",
    "turbine": "\u2699\ufe0f",
    "flow": "\U0001f30a",
    "seepage": "\U0001f53d",
    "pump": "\U0001f4a7\u2191",  # water drop + up arrow
    "volume_right": "\U0001f4d0",  # bookmark
    "flow_right": "\U0001f4c9",  # chart with downwards trend
    "lng_terminal": "\U0001f525",  # flame / gas
    # aggregated generator nodes inherit the type icon
    "gen_wind": "\U0001f32c\ufe0f",
    "gen_nuclear": "\u2622\ufe0f",
    "gen_gas": "\U0001f525",
}

_FA_CDN = "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.2/css/all.min.css"

_PYVIS_COLORS: dict[str, dict] = {
    "bus": {"background": "#E8F5E9", "border": "#2E7D32"},
    "gen": {"background": "#FEF9E7", "border": "#E67E22"},
    "gen_solar": {"background": "#FDEBD0", "border": "#F39C12"},
    "gen_hydro": {"background": "#E3F2FD", "border": "#1565C0"},
    "demand": {"background": "#FADBD8", "border": "#C0392B"},
    "battery": {"background": "#F4ECF7", "border": "#7D3C98"},
    "converter": {"background": "#E8F5E9", "border": "#388E3C"},
    "junction": {"background": "#E3F2FD", "border": "#1565C0"},
    "reservoir": {"background": "#E1F5FE", "border": "#01579B"},
    "reservoir_sm": {"background": "#E1F5FE", "border": "#4FC3F7"},
    "reservoir_md": {"background": "#81D4FA", "border": "#0288D1"},
    "reservoir_lg": {"background": "#039BE5", "border": "#01579B"},
    "turbine": {"background": "#BBDEFB", "border": "#1565C0"},
    "flow": {"background": "#E3F2FD", "border": "#0277BD"},
    "seepage": {"background": "#E0E0E0", "border": "#616161"},
    "pump": {"background": "#E3F2FD", "border": "#0D47A1"},
    "volume_right": {"background": "#EFEBE9", "border": "#6D4C41"},
    "flow_right": {"background": "#E0F7FA", "border": "#00838F"},
    "lng_terminal": {"background": "#E8EAF6", "border": "#01579B"},
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
    "seepage": "dot",
    "pump": "diamond",
    "volume_right": "box",
    "flow_right": "dot",
    "lng_terminal": "database",
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
    "seepage": 14,
    "pump": 22,
    "volume_right": 18,
    "flow_right": 14,
    "lng_terminal": 26,
}

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
    "seepage": "circle",
    "pump": "diamond",
    "volume_right": "folder",
    "flow_right": "circle",
    "lng_terminal": "cylinder",
    "reserve_zone": "star",
    "gen_profile": "note",
    "dem_profile": "note",
}

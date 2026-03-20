# SPDX-License-Identifier: BSD-3-Clause
"""Pure-logic helpers for generator classification, element naming, and scalar formatting.

These functions are intentionally free of I/O, GUI, or Graphviz dependencies so
they can be unit-tested in isolation.  The main ``gtopt_diagram`` module imports
them and re-exports the private names for backward compatibility.
"""

from __future__ import annotations

from typing import Optional


# ---------------------------------------------------------------------------
# Generator-type classification metadata
# ---------------------------------------------------------------------------

# Map internal type key -> (display label, mermaid-icon, palette-key)
GEN_TYPE_META: dict[str, tuple[str, str, str]] = {
    "hydro": ("Hydro", "\U0001f4a7", "gen_hydro"),
    "solar": ("Solar", "\u2600\ufe0f", "gen_solar"),
    "wind": ("Wind", "\U0001f32c\ufe0f", "gen_solar"),  # reuse solar palette
    "battery": ("BESS", "\U0001f50b", "battery"),
    "thermal": ("Thermal", "\u26a1", "gen"),
}


# ---------------------------------------------------------------------------
# Turbine / efficiency reference collectors
# ---------------------------------------------------------------------------


def turbine_gen_refs(sys: dict) -> set:
    """Return the set of generator uid/name references that have a turbine."""
    refs: set = set()
    for t in sys.get("turbine_array", []):
        g = t.get("generator")
        if g is not None:
            refs.add(g)
    return refs


def turbine_waterway_refs(sys: dict) -> set:
    """Return the set of waterway uid/name references that have a turbine.

    Used to suppress the direct ``junction_a -> junction_b`` edge for waterways
    that already have a turbine node representing the arc.
    """
    refs: set = set()
    for t in sys.get("turbine_array", []):
        w = t.get("waterway")
        if w is not None:
            refs.add(w)
    return refs


def efficiency_turbine_pairs(sys: dict) -> set:
    """Return the set of turbine uid/name references covered by reservoir_efficiency_array.

    Used to avoid drawing a duplicate ``main_reservoir`` edge when a
    ``reservoir_efficiency_array`` entry already represents the same
    turbine-reservoir relationship.
    """
    refs: set = set()
    for e in sys.get("reservoir_efficiency_array", []):
        t = e.get("turbine")
        if t is not None:
            refs.add(t)
    return refs


# ---------------------------------------------------------------------------
# Generator classification
# ---------------------------------------------------------------------------


def classify_gen(gen: dict, turb_refs: set) -> str:
    """Classify a generator as 'hydro', 'solar', 'wind', 'battery', or 'thermal'."""
    uid = gen.get("uid")
    name_ref = gen.get("name")
    name = str(name_ref or "").lower()
    if uid in turb_refs or name_ref in turb_refs:
        return "hydro"
    if any(k in name for k in ("solar", "pv", "foto", "fotov", "cspv")):
        return "solar"
    if any(k in name for k in ("wind", "eol", "eólico", "eolico", "aerog")):
        return "wind"
    if any(k in name for k in ("bat", "bess", "ess", "storage", "almac")):
        return "battery"
    return "thermal"


# ---------------------------------------------------------------------------
# Generator pmax extraction
# ---------------------------------------------------------------------------


def gen_pmax(gen: dict) -> float:
    """Extract the effective pmax from a generator dict (scalar or nested list)."""
    v = gen.get("pmax") or gen.get("capacity")
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, list):
        flat: list[float] = []

        def _flatten_numeric(x):  # noqa: ANN001
            if isinstance(x, list):
                for i in x:
                    _flatten_numeric(i)
            elif isinstance(x, (int, float)):
                flat.append(float(x))

        _flatten_numeric(v)
        return max(flat) if flat else 0.0
    return 0.0


# ---------------------------------------------------------------------------
# Element naming
# ---------------------------------------------------------------------------


def elem_name(item: dict) -> str:
    """Return a display label combining name and uid: ``'NAME:UID'``.

    Uses colon separator instead of parentheses to avoid Mermaid flowchart
    syntax errors (parentheses are reserved for node shapes in Mermaid).

    Examples:
      - ``{"name": "ELTORO", "uid": 2}``  -> ``"ELTORO:2"``
      - ``{"name": "b1",     "uid": 1}``  -> ``"b1:1"``
      - ``{"name": "b1"}``                -> ``"b1"``
      - ``{"uid": 3}``                    -> ``"3"``
      - ``{}``                            -> ``"?"``
    """
    name = item.get("name")
    uid = item.get("uid")
    if name is not None and uid is not None and str(name) != str(uid):
        return f"{name}:{uid}"
    if name is not None:
        return str(name)
    if uid is not None:
        return str(uid)
    return "?"


# ---------------------------------------------------------------------------
# Scalar formatting
# ---------------------------------------------------------------------------


def scalar(v) -> str:  # noqa: ANN001
    """Format a JSON value for display (numbers to 2 dp, lists as ranges)."""
    if v is None:
        return "\u2014"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if v == int(v):
            return str(int(v))
        return f"{v:.2f}"
    if isinstance(v, list):
        flat: list = []

        def _flatten_values(x):  # noqa: ANN001
            if isinstance(x, list):
                for i in x:
                    _flatten_values(i)
            else:
                flat.append(x)

        _flatten_values(v)
        if flat:
            mn, mx = min(flat), max(flat)
            return f"{scalar(mn)}\u2026{scalar(mx)}" if mn != mx else scalar(mn)
        return "\u2014"
    if isinstance(v, str):
        return f'"{v}"'
    return str(v)


# ---------------------------------------------------------------------------
# Array lookup by uid/name
# ---------------------------------------------------------------------------


def resolve(arr: list[dict], ref) -> Optional[dict]:  # noqa: ANN001
    """Find the first element in *arr* whose uid or name matches *ref*."""
    for item in arr:
        if item.get("uid") == ref or item.get("name") == ref:
            return item
    return None


# ---------------------------------------------------------------------------
# Generator-type icon helpers
# ---------------------------------------------------------------------------


def icon_key_for_type(gen_type: str) -> str:
    """Return the palette/icon key for a given generator type string."""
    return {
        "hydro": "gen_hydro",
        "solar": "gen_solar",
        "wind": "gen_solar",
        "battery": "battery",
        "nuclear": "gen",
        "gas": "gen",
        "thermal": "gen",
    }.get(gen_type, "gen")


def dominant_kind(types: list) -> str:
    """Return the palette-key for the most prominent generator type in *types*.

    Priority order: hydro > solar > wind > battery > thermal.
    """
    counts: dict = {}
    for t in types:
        counts[t] = counts.get(t, 0) + 1
    for p in ["hydro", "solar", "wind", "battery", "thermal"]:
        if counts.get(p, 0) > 0:
            return GEN_TYPE_META.get(p, ("?", "\u26a1", "gen"))[2]
    return "gen"

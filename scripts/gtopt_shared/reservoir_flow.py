# SPDX-License-Identifier: BSD-3-Clause
"""Topology-driven reservoir extraction-flow estimator (issue #5xx).

gtopt reservoirs that ship without concrete ``fmin`` / ``fmax`` fall back
to the C++ :class:`ReservoirLP` generic defaults of ``-9000`` / ``6000``
m³/s for the extraction column (see ``source/reservoir_lp.cpp``).  Those
free-below columns are wide enough to block GPU first-order / heuristic
LP solvers and they carry no physical meaning.

This module replaces them with **concrete, per-reservoir** bounds
estimated from the hydraulic network so the assembled LP has tight,
physically grounded reservoir extraction bounds.

Data model (gtopt planning JSON ``planning["system"]``)
-------------------------------------------------------
* ``reservoir_array``: ``{uid, name, junction, ..., [fmin], [fmax]}`` —
  a reservoir sits at a junction (referenced by *name* OR integer *uid*).
* ``waterway_array``: ``{uid, name, junction_a, junction_b, fmax}`` — a
  directed arc ``junction_a → junction_b`` with capacity ``fmax`` [m³/s].
  ``fmax`` may be numeric, a string parquet-table reference, or absent.
* ``turbine_array``: a built-in waterway.  Two topology modes:
  - **own arc**: turbine carries its own ``junction_a`` (+ optional
    ``junction_b``).
  - **waterway-link**: turbine references a ``waterway`` by name and
    inherits that waterway's ``junction_a`` / ``junction_b``.
  Max flow [m³/s] = ``generator_pmax / production_factor``.  A turbine
  with no ``junction_b`` (either mode) is *terminal* — it drains out of
  the hydro system.
* ``flow_array``: ``{uid, name, junction, discharge, [direction]}`` —
  natural inflow (or forced flow) at a junction.  ``discharge`` is a
  field-schedule: a scalar, an inline nested array (scenario × stage ×
  block), OR a string naming a parquet table (plp2gtopt uses the string
  ref, plexos2gtopt uses inline arrays).
* ``junction_array``: ``{uid, name, [drain], [drain_capacity]}``.
* ``generator_array``: ``{uid, name, pmax, ...}``.

All references resolve by **name OR integer uid** — both are handled.

Algorithm
---------
For each reservoir ``R`` at junction ``J``:

``max_release`` (→ ``fmax``, the positive RELEASE-downstream side) — the
total capacity that can carry water OUT of ``J``:

* Σ ``fmax`` of waterways with ``junction_a == J`` (a string parquet-ref
  ``fmax`` resolves from ``<input_dir>/Waterway/fmax.parquet``)
* Σ turbine max-flow (``gen_pmax / production_factor``) of turbines whose
  upstream junction is ``J`` (a string parquet-ref ``pmax`` resolves from
  ``<input_dir>/Generator/pmax.parquet``)
* ``+ drain_capacity`` if ``J`` has ``drain`` AND ships an explicit numeric
  ``drain_capacity``.  A draining junction with NO numeric
  ``drain_capacity`` is an UNBOUNDED spillway: the reservoir's release is
  not topology-limited, so ``fmax`` is left UNSET (→ C++ ``DblMax``),
  mirroring plexos2gtopt.  The unbounded drain spills OUT to a sink and is
  excluded from the finite super-source edges that feed downstream.

``max_inflow`` (→ ``fmin = -max_inflow``, the ACCEPT-inflow side) — the
network-bottlenecked max water that can ARRIVE at ``J``, via a NetworkX
max-flow on a directed graph of junctions with capacitated edges:

* waterways (``fmax``) and non-terminal turbines (``gen_pmax / pf``);
  terminal turbines edge to a synthetic ``__SINK__``.
* a synthetic super-source ``__SRC__`` feeds: (a) every junction carrying
  a natural inflow, capacity = peak inflow there; (b) every *other*
  reservoir's junction, capacity = that reservoir's own ``max_release``.

For reservoir ``R`` the ``__SRC__ → J_R`` reservoir-release edge is
dropped (so ``R`` is not its own source) while ``R`` still receives its
own natural inflow.  ``max_inflow(R) = maximum_flow_value(__SRC__, J_R)``.

Bounds are set ONLY when the field is absent / sentinel-infinite AND the
estimate is finite & strictly positive; otherwise the field is left
unset and the C++ default applies.  A small safety margin
(:data:`SAFETY_MARGIN`, ×1.1) is applied so the bound never clips a
feasible operating point.  Explicit, finite user-provided bounds are
never overridden.
"""

from __future__ import annotations

import logging
import math
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

#: Values with magnitude at or above this are gtopt's effective-infinity
#: sentinel (``±1e30``) — treated as "absent" so the estimate replaces them.
INFINITY_SENTINEL: float = 1.0e29

#: Multiplicative safety margin on every estimated bound so the bound
#: never clips a feasible operating point (rounding + small modelling
#: head-room).  Applied before rounding.
SAFETY_MARGIN: float = 1.1

_SRC = "__SRC__"
_SINK = "__SINK__"


def _is_absent_bound(value: Any) -> bool:
    """Return True when *value* should be treated as an unset bound.

    Absent = ``None`` / missing, a non-numeric (e.g. a parquet-ref
    string), or a sentinel-infinite magnitude (``±1e30``).
    """
    if value is None:
        return True
    if isinstance(value, bool):  # bool is an int subclass — never a bound
        return True
    if not isinstance(value, (int, float)):
        return True
    return not math.isfinite(value) or abs(float(value)) >= INFINITY_SENTINEL


def _numeric_cap(value: Any) -> float | None:
    """Return a finite positive capacity from *value*, else ``None``.

    Resolves a scalar OR an inline time-series (nested list) to its PEAK
    (max leaf) — the conservative outer-bound capacity (a derated /
    DLR-profiled ``pmax`` or ``fmax`` is common, and its peak is the
    largest flow it can ever carry, so it never clips a feasible point).
    Returns ``None`` for a string parquet reference (resolved separately),
    a non-finite / ``±1e30`` sentinel, or a non-positive cap.
    """
    f = _peak_from_schedule(value)
    if f is None or not math.isfinite(f) or abs(f) >= INFINITY_SENTINEL:
        return None
    return f if f > 0.0 else None


def _min_positive_cap(value: Any) -> float | None:
    """Smallest positive leaf of a scalar / inline time-series, else ``None``.

    Used for ``production_factor`` so that turbine max-flow
    ``pmax_peak / pf_min`` is the conservative (largest) flow over a
    time-varying production factor.  String parquet refs → ``None``.
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        f = float(value)
        return f if math.isfinite(f) and f > 0.0 else None
    if isinstance(value, (list, tuple)):
        best: float | None = None
        stack: list[Any] = list(value)
        while stack:
            item = stack.pop()
            if isinstance(item, (list, tuple)):
                stack.extend(item)
            elif isinstance(item, (int, float)) and not isinstance(item, bool):
                f = float(item)
                if math.isfinite(f) and f > 0.0 and (best is None or f < best):
                    best = f
        return best
    return None


def _junction_key(
    ref: Any, name_to_key: dict[str, str], uid_to_key: dict[int, str]
) -> str | None:
    """Resolve a junction reference (name or uid) to its canonical key.

    The canonical key is the junction *name* when available, else the
    string of its uid.  Returns ``None`` when the reference cannot be
    resolved (e.g. a synthetic junction not in ``junction_array`` — those
    are added on demand by the caller).
    """
    if ref is None:
        return None
    if isinstance(ref, str):
        return name_to_key.get(ref, ref)
    if isinstance(ref, int):
        return uid_to_key.get(ref)
    return None


def _build_index_maps(
    elements: list[dict[str, Any]],
) -> tuple[dict[str, Any], dict[int, Any]]:
    """Return ``(name → element, uid → element)`` maps for *elements*."""
    by_name: dict[str, Any] = {}
    by_uid: dict[int, Any] = {}
    for el in elements:
        name = el.get("name")
        uid = el.get("uid")
        if isinstance(name, str):
            by_name[name] = el
        if isinstance(uid, int) and not isinstance(uid, bool):
            by_uid[uid] = el
    return by_name, by_uid


def _turbine_maxflow_key(turbine: dict[str, Any]) -> str:
    """Stable lookup key for a turbine's resolved max-flow.

    Prefer the turbine name, fall back to ``uid:<n>`` so anonymous
    turbines still resolve.
    """
    name = turbine.get("name")
    if isinstance(name, str):
        return name
    return f"uid:{turbine.get('uid')}"


def _turbine_junctions(
    turbine: dict[str, Any],
    waterway_by_name: dict[str, Any],
    waterway_by_uid: dict[int, Any],
) -> tuple[Any, Any]:
    """Resolve ``(junction_a, junction_b)`` for a turbine.

    A turbine carries its own ``junction_a`` (built-in waterway mode), or
    references a ``waterway`` by name/uid and inherits that waterway's
    junctions (waterway-link mode).  ``junction_b`` may be ``None``
    (terminal drain).
    """
    if turbine.get("junction_a") is not None:
        return turbine.get("junction_a"), turbine.get("junction_b")
    wref = turbine.get("waterway")
    ww: Any = None
    if isinstance(wref, str):
        ww = waterway_by_name.get(wref)
    elif isinstance(wref, int) and not isinstance(wref, bool):
        ww = waterway_by_uid.get(wref)
    if ww is not None:
        return ww.get("junction_a"), ww.get("junction_b")
    return None, None


def _pf_at_volume(segments: list[dict[str, Any]], volume: float) -> float | None:
    """Production factor at reservoir *volume* via the PLP concave envelope.

    Implements ``FRendimientos`` (``plp-frendim.f`` / ``cenre_parser.py``):
    the production factor at volume ``V`` is the concave-envelope minimum
    over piecewise-linear segments::

        PF(V) = min_i ( constant_i + slope_i * (V - volume_i) )

    where ``constant_i`` is the PF AT breakpoint ``volume_i`` (point-slope
    form — *not* a y-intercept).  Returns ``None`` when no finite segment
    contributes.
    """
    best: float | None = None
    for seg in segments:
        slope = seg.get("slope")
        const = seg.get("constant")
        v0 = seg.get("volume", 0.0)
        if not (
            isinstance(slope, (int, float))
            and not isinstance(slope, bool)
            and isinstance(const, (int, float))
            and not isinstance(const, bool)
            and isinstance(v0, (int, float))
            and not isinstance(v0, bool)
        ):
            continue
        pf = float(const) + float(slope) * (volume - float(v0))
        if math.isfinite(pf) and (best is None or pf < best):
            best = pf
    return best


def resolve_min_production_factors(system: dict[str, Any]) -> dict[str, float]:
    """Resolve the minimum head-dependent production factor per turbine.

    For each ``reservoir_production_factor`` element, resolve the referenced
    reservoir's volume range ``[emin, emax]`` from ``reservoir_array``,
    evaluate ``PF(emin)`` and ``PF(emax)`` via the PLP concave-envelope
    formula (:func:`_pf_at_volume`), and take ``min_PF = min(PF(emin),
    PF(emax))``.  Because ``PF`` is concave (a min of linear pieces) its
    minimum over ``[emin, emax]`` is attained at an endpoint, so the two
    endpoints suffice.

    Returns a map keyed by the turbine reference as **both** its name and
    ``str(uid)`` so a downstream lookup resolves regardless of how the
    turbine is keyed.  When a turbine has multiple ``reservoir_production_factor``
    elements the smallest ``min_PF`` across them wins (most conservative).

    A ``min_PF`` that is non-finite or ``<= 0`` is skipped.  When an
    element has no usable ``segments`` it falls back to
    ``mean_production_factor`` (if ``> 0``).
    """
    elements = system.get("reservoir_production_factor_array", []) or []
    reservoirs = system.get("reservoir_array", []) or []
    res_by_name, res_by_uid = _build_index_maps(reservoirs)

    out: dict[str, float] = {}
    for el in elements:
        reservoir = _lookup_ref(el.get("reservoir"), res_by_name, res_by_uid)
        segments = el.get("segments") or []
        min_pf: float | None = None
        if reservoir is not None and segments:
            emin = reservoir.get("emin", 0.0)
            emin = float(emin) if isinstance(emin, (int, float)) else 0.0
            emax_raw = reservoir.get("emax")
            if not isinstance(emax_raw, (int, float)) or isinstance(emax_raw, bool):
                emax_raw = reservoir.get("capacity")
            emax = (
                float(emax_raw)
                if isinstance(emax_raw, (int, float)) and not isinstance(emax_raw, bool)
                else emin
            )
            pf_lo = _pf_at_volume(segments, emin)
            pf_hi = _pf_at_volume(segments, emax)
            candidates = [p for p in (pf_lo, pf_hi) if p is not None]
            if candidates:
                min_pf = min(candidates)
        if min_pf is None or not math.isfinite(min_pf) or min_pf <= 0.0:
            # Fall back to the scalar mean rendimiento when segments are
            # absent / unusable.
            mean_pf = el.get("mean_production_factor")
            if (
                isinstance(mean_pf, (int, float))
                and not isinstance(mean_pf, bool)
                and math.isfinite(mean_pf)
                and mean_pf > 0.0
            ):
                min_pf = float(mean_pf)
            else:
                continue
        for key in _ref_keys(el.get("turbine")):
            existing = out.get(key)
            out[key] = min_pf if existing is None else min(existing, min_pf)
    return out


def _lookup_ref(ref: Any, by_name: dict[str, Any], by_uid: dict[int, Any]) -> Any:
    """Resolve a name-or-uid reference against name/uid index maps."""
    if isinstance(ref, str):
        return by_name.get(ref)
    if isinstance(ref, int) and not isinstance(ref, bool):
        return by_uid.get(ref)
    return None


def _ref_keys(ref: Any) -> list[str]:
    """Lookup keys for a name-or-uid reference (both name and ``str(uid)``)."""
    if isinstance(ref, str):
        return [ref]
    if isinstance(ref, int) and not isinstance(ref, bool):
        return [str(ref)]
    return []


def compute_turbine_maxflow(
    system: dict[str, Any],
    *,
    generator_peaks: dict[str, float] | None = None,
    min_production_factors: dict[str, float] | None = None,
    generator_capacities: dict[Any, float] | None = None,
    extra_turbines: list[dict[str, Any]] | None = None,
) -> dict[str, float]:
    """Compute per-turbine max flow [m³/s] = ``gen_pmax / production_factor``.

    Returns a map keyed by :func:`_turbine_maxflow_key`.  Turbines whose
    generator ``pmax`` is unresolved (string ref with no resolved peak, or
    missing) or whose ``production_factor`` is non-positive are skipped.

    *min_production_factors* (from :func:`resolve_min_production_factors`)
    supplies the authoritative, head-dependent *minimum* production factor
    keyed by turbine name OR ``str(uid)``.  When present (and ``> 0``) it
    takes precedence over the turbine's scalar ``production_factor`` so the
    resulting max flow ``pmax / min_PF`` is conservative; otherwise the
    scalar is used as a fallback.

    *generator_capacities* (keyed by generator ref / uid / name) overrides
    the JSON dispatch ``pmax`` with the generator's PHYSICAL NAMEPLATE.  This
    matters when the dispatch ``pmax`` is reduced by a weekly maintenance
    schedule (PLEXOS zeroes ``Gen_Rating`` for out-of-service units): the
    reservoir extraction bound is structural and must reflect ALL units'
    nameplate, not just the units available in one horizon — while the
    dispatch ``pmax`` stays untouched.
    """
    turbines = list(system.get("turbine_array", []) or []) + list(extra_turbines or [])
    generators = system.get("generator_array", []) or []
    gen_by_name, gen_by_uid = _build_index_maps(generators)
    peaks = generator_peaks or {}
    min_pfs = min_production_factors or {}
    gen_caps = generator_capacities or {}

    out: dict[str, float] = {}
    for t in turbines:
        pf = _resolve_turbine_pf(t, min_pfs)
        if pf is None:
            continue
        gref = t.get("generator")
        gen: Any = None
        if isinstance(gref, str):
            gen = gen_by_name.get(gref)
        elif isinstance(gref, int) and not isinstance(gref, bool):
            gen = gen_by_uid.get(gref)
        # Prefer the physical nameplate (all units) over the dispatch pmax.
        pmax = _generator_capacity_override(gref, gen, gen_caps)
        if pmax is None and gen is not None:
            pmax = _numeric_cap(gen.get("pmax"))
            if pmax is None and isinstance(gref, str):
                pmax = peaks.get(gref)
        if pmax is None:
            # Fallback: the turbine's own ``capacity`` field is the
            # already-resolved max flow [m³/s] (= gen_pmax / pf, stamped
            # upstream by the hydro writer).  Use it when the generator
            # ``pmax`` is an unresolved parquet-ref string, so reservoirs
            # like RALCO (``gen pmax == 'pmax'`` string) still contribute
            # their FULL turbine release to ``max_release``.  Without this
            # the estimator omits the turbine entirely and sets the
            # reservoir extraction bound far too tight (e.g. RALCO ±60 vs
            # a 438 m³/s turbine) — a dry-hydrology solve then goes
            # infeasible because the node balance cannot pass the inflow
            # through a spuriously-capped storage flow (PLP leaves qe
            # unbounded).  ``capacity`` is already a flow, so no ``/ pf``.
            cap = _numeric_cap(t.get("capacity"))
            if cap is not None:
                out[_turbine_maxflow_key(t)] = cap
            continue
        out[_turbine_maxflow_key(t)] = pmax / pf
    return out


def _generator_capacity_override(
    gref: Any, gen: dict[str, Any] | None, overrides: dict[Any, float]
) -> float | None:
    """Nameplate capacity override for a generator (by ref, then uid/name)."""
    if not overrides:
        return None
    cap = overrides.get(gref) if not isinstance(gref, bool) else None
    if cap is None and gen is not None:
        uid = gen.get("uid")
        if isinstance(uid, int) and not isinstance(uid, bool):
            cap = overrides.get(uid)
        if cap is None:
            nm = gen.get("name")
            if isinstance(nm, str):
                cap = overrides.get(nm)
    return float(cap) if isinstance(cap, (int, float)) and cap > 0.0 else None


def _resolve_turbine_pf(
    turbine: dict[str, Any], min_pfs: dict[str, float]
) -> float | None:
    """Production factor for a turbine: head-dependent min, else scalar.

    Prefers the authoritative head-dependent minimum from
    ``min_pfs`` (keyed by turbine name OR ``str(uid)``) when present and
    ``> 0``; otherwise falls back to the smallest positive leaf of the
    turbine's scalar / time-series ``production_factor``.  Returns ``None``
    when neither yields a positive value.
    """
    if min_pfs:
        name = turbine.get("name")
        pf = min_pfs.get(name) if isinstance(name, str) else None
        if pf is None:
            uid = turbine.get("uid")
            if isinstance(uid, int) and not isinstance(uid, bool):
                pf = min_pfs.get(str(uid))
        if pf is not None and pf > 0.0:
            return float(pf)
    return _min_positive_cap(turbine.get("production_factor"))


def _peak_from_schedule(value: Any) -> float | None:
    """Peak (max leaf) of an inline schedule (scalar or nested list).

    Returns ``None`` for a string parquet reference (resolved separately)
    or when no finite leaf is found.
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value) if math.isfinite(value) else None
    if isinstance(value, str):
        return None
    if isinstance(value, (list, tuple)):
        best: float | None = None
        stack: list[Any] = list(value)
        while stack:
            item = stack.pop()
            if isinstance(item, (list, tuple)):
                stack.extend(item)
            elif isinstance(item, (int, float)) and not isinstance(item, bool):
                f = float(item)
                if math.isfinite(f) and (best is None or f > best):
                    best = f
        return best
    return None


def _read_parquet_peaks_by_uid(path: Path) -> dict[int, float] | None:
    """Read a flow parquet table and return ``{flow_uid: peak_value}``.

    Handles both layouts plp2gtopt may emit:

    * **long** — columns ``scenario, stage, block, uid, value`` (the
      ``uid`` column holds the flow uid).
    * **wide** — index columns plus ``uid:<n>`` / ``<name>:<n>`` value
      columns (one per flow); the integer suffix is the flow uid.

    Returns ``None`` (with a warning) when the file is missing or
    unreadable so the caller degrades gracefully.
    """
    if not path.exists():
        logger.warning("reservoir_flow: discharge parquet not found: %s", path)
        return None
    try:
        import pyarrow.parquet as pq  # noqa: PLC0415  pylint: disable=import-outside-toplevel
    except ImportError:  # pragma: no cover - pyarrow is a hard dep in practice
        logger.warning("reservoir_flow: pyarrow unavailable; cannot read %s", path)
        return None
    try:
        table = pq.read_table(path)
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        logger.warning("reservoir_flow: failed to read %s: %s", path, exc)
        return None

    cols = table.column_names
    peaks: dict[int, float] = {}
    if "uid" in cols and "value" in cols:
        uids = table.column("uid").to_pylist()
        vals = table.column("value").to_pylist()
        for uid, val in zip(uids, vals):
            if uid is None or val is None:
                continue
            try:
                u = int(uid)
                v = float(val)
            except (TypeError, ValueError):
                continue
            if math.isfinite(v) and (u not in peaks or v > peaks[u]):
                peaks[u] = v
        return peaks

    # Wide layout: parse uid out of each value column's name.
    index_cols = {"scenario", "stage", "block"}
    for name in cols:
        if name in index_cols:
            continue
        suffix = name.rsplit(":", 1)[-1]
        try:
            u = int(suffix)
        except ValueError:
            continue
        column = table.column(name).to_pylist()
        best: float | None = None
        for val in column:
            if val is None:
                continue
            try:
                v = float(val)
            except (TypeError, ValueError):
                continue
            if math.isfinite(v) and (best is None or v > best):
                best = v
        if best is not None:
            peaks[u] = best
    return peaks


def resolve_inflow_peaks(
    system: dict[str, Any],
    *,
    input_dir: Path | None,
) -> dict[str, float]:
    """Resolve the peak inflow per junction key from ``flow_array``.

    Handles all three schedule forms:

    * scalar → the value
    * inline nested list → the max leaf
    * string parquet-table ref → read ``<input_dir>/Flow/<table>.parquet``
      (or ``<input_dir>/<table>.parquet``) and take the per-flow max,
      keyed by flow uid.

    Peaks of multiple flows that share a junction are summed.  An
    unresolvable parquet ref logs a warning and contributes 0 (never
    crashes).

    Only *natural inflow* directions contribute.  ``direction`` semantics
    are ambiguous in the current data model, so to stay conservative we
    treat a flow as an inflow unless ``direction`` is explicitly one of
    the recognised outflow tokens (``"out"`` / ``"outflow"`` /
    ``"extraction"`` / ``-1``); those are skipped here (they feed the
    release side, handled elsewhere only when unambiguous).
    """
    flows = system.get("flow_array", []) or []
    junctions = system.get("junction_array", []) or []
    name_to_key, uid_to_key = _junction_lookup(junctions)

    # Lazily-read parquet tables: table-name → {flow_uid: peak}.
    table_cache: dict[str, dict[int, float] | None] = {}

    peaks: dict[str, float] = {}
    for flow in flows:
        direction = flow.get("direction")
        if _is_outflow_direction(direction):
            continue
        jkey = _junction_key(flow.get("junction"), name_to_key, uid_to_key)
        if jkey is None:
            jref = flow.get("junction")
            jkey = jref if isinstance(jref, str) else str(jref)
        discharge = flow.get("discharge")
        peak = _peak_from_schedule(discharge)
        if peak is None and isinstance(discharge, str):
            peak = _resolve_parquet_peak(discharge, flow, input_dir, table_cache)
        if peak is None or peak <= 0.0:
            continue
        peaks[jkey] = peaks.get(jkey, 0.0) + peak
    return peaks


def _resolve_parquet_peak(
    table_name: str,
    flow: dict[str, Any],
    input_dir: Path | None,
    table_cache: dict[str, dict[int, float] | None],
) -> float | None:
    """Resolve one flow's peak from its referenced parquet table."""
    if input_dir is None:
        logger.warning(
            "reservoir_flow: flow %r references parquet table %r but no "
            "input_dir was supplied; treating peak as 0",
            flow.get("name"),
            table_name,
        )
        return None
    if table_name not in table_cache:
        table_cache[table_name] = _locate_and_read_table(
            table_name, input_dir, subdir="Flow"
        )
    by_uid = table_cache[table_name]
    if not by_uid:
        return None
    uid = flow.get("uid")
    if isinstance(uid, int) and not isinstance(uid, bool):
        return by_uid.get(uid)
    return None


def _read_csv_peaks_by_uid(path: Path) -> dict[int, float] | None:
    """Read a long-layout CSV time-series and return ``{uid: peak}``.

    Columns ``scenario|block, stage, uid, value`` (the ``value`` peak per
    ``uid``).  The CSV counterpart of :func:`_read_parquet_peaks_by_uid`
    so a ``-F csv`` conversion resolves ``pmax`` / ``fmax`` peaks exactly
    like a parquet one — without this, string-ref bounds (e.g. ELTORO's
    ``gen pmax == 'pmax'``) never resolve and the extraction estimate is
    left far too tight.
    """
    try:
        import csv  # noqa: PLC0415  pylint: disable=import-outside-toplevel

        peaks: dict[int, float] = {}
        with open(path, newline="", encoding="utf-8") as fh:
            reader = csv.DictReader(fh)
            if reader.fieldnames is None or "uid" not in reader.fieldnames:
                return None
            for row in reader:
                try:
                    u = int(row["uid"])
                    v = float(row["value"])
                except (TypeError, ValueError, KeyError):
                    continue
                if math.isfinite(v) and (u not in peaks or v > peaks[u]):
                    peaks[u] = v
        return peaks
    except OSError as exc:
        logger.warning("reservoir_flow: failed to read %s: %s", path, exc)
        return None


def _locate_and_read_table(
    table_name: str, input_dir: Path | None, *, subdir: str = "Flow"
) -> dict[int, float] | None:
    """Find ``<table>.{parquet,csv}`` under common locations, read peaks.

    Tries ``<input_dir>/<subdir>/<table>.parquet`` first (the per-element
    class directory, e.g. ``Generator/pmax.parquet``,
    ``Waterway/fmax.parquet``, ``Flow/discharge.parquet``), then the
    flat ``<input_dir>/<table>.parquet`` fallback, then the ``.csv``
    variants (a ``-F csv`` conversion emits CSV time-series).  Keyed by
    element uid.
    """
    if input_dir is None:
        return None
    for path in (
        input_dir / subdir / f"{table_name}.parquet",
        input_dir / f"{table_name}.parquet",
    ):
        if path.exists():
            return _read_parquet_peaks_by_uid(path)
    for path in (
        input_dir / subdir / f"{table_name}.csv",
        input_dir / f"{table_name}.csv",
    ):
        if path.exists():
            return _read_csv_peaks_by_uid(path)
    logger.warning(
        "reservoir_flow: could not locate table %r (.parquet/.csv) under "
        "%s; treating peak as 0",
        table_name,
        input_dir,
    )
    return None


def resolve_generator_pmax_peaks(
    system: dict[str, Any],
    *,
    input_dir: Path | None,
) -> dict[str, float]:
    """Resolve peak ``pmax`` [MW] per generator from a parquet table ref.

    plp2gtopt emits generator ``pmax`` as the string ``"pmax"`` (a
    reference to ``<input_dir>/Generator/pmax.parquet``, long-layout keyed
    by **generator uid**).  This reads that table once and returns a map
    ``{generator_name: peak, str(generator_uid): peak}`` so a turbine that
    references its generator by *either* name or uid resolves.

    Inline / scalar ``pmax`` generators are skipped here (resolved by
    :func:`_numeric_cap` directly in :func:`compute_turbine_maxflow`).
    Returns an empty map when ``input_dir`` is ``None`` or the table is
    missing (plexos2gtopt path keeps inline values).
    """
    generators = system.get("generator_array", []) or []
    if input_dir is None:
        return {}

    table_cache: dict[str, dict[int, float] | None] = {}
    out: dict[str, float] = {}
    for gen in generators:
        pmax = gen.get("pmax")
        if not isinstance(pmax, str):
            continue  # inline value resolved directly elsewhere
        if pmax not in table_cache:
            table_cache[pmax] = _locate_and_read_table(
                pmax, input_dir, subdir="Generator"
            )
        by_uid = table_cache[pmax]
        if not by_uid:
            continue
        uid = gen.get("uid")
        if not (isinstance(uid, int) and not isinstance(uid, bool)):
            continue
        peak = by_uid.get(uid)
        if peak is None or not math.isfinite(peak) or peak <= 0.0:
            continue
        name = gen.get("name")
        if isinstance(name, str):
            out[name] = peak
        out[str(uid)] = peak
    return out


def resolve_waterway_fmax_peaks(
    system: dict[str, Any],
    *,
    input_dir: Path | None,
) -> dict[int, float]:
    """Resolve peak ``fmax`` [m³/s] per waterway from a parquet table ref.

    plp2gtopt emits some waterway ``fmax`` as the string ``"fmax"`` (a
    reference to ``<input_dir>/Waterway/fmax.parquet``, long-layout keyed
    by **waterway uid**).  Returns ``{waterway_uid: peak}``.

    Inline / scalar ``fmax`` waterways are skipped here (resolved by
    :func:`_numeric_cap`).  Returns an empty map when ``input_dir`` is
    ``None`` or the table is missing.
    """
    waterways = system.get("waterway_array", []) or []
    if input_dir is None:
        return {}

    table_cache: dict[str, dict[int, float] | None] = {}
    out: dict[int, float] = {}
    for w in waterways:
        fmax = w.get("fmax")
        if not isinstance(fmax, str):
            continue
        if fmax not in table_cache:
            table_cache[fmax] = _locate_and_read_table(
                fmax, input_dir, subdir="Waterway"
            )
        by_uid = table_cache[fmax]
        if not by_uid:
            continue
        uid = w.get("uid")
        if not (isinstance(uid, int) and not isinstance(uid, bool)):
            continue
        peak = by_uid.get(uid)
        if peak is None or not math.isfinite(peak) or peak <= 0.0:
            continue
        out[uid] = peak
    return out


_OUTFLOW_DIRECTIONS = frozenset({"out", "outflow", "extraction", "draw"})


def _is_outflow_direction(direction: Any) -> bool:
    """Return True when *direction* unambiguously marks an outflow draw."""
    if isinstance(direction, str):
        return direction.strip().lower() in _OUTFLOW_DIRECTIONS
    if isinstance(direction, (int, float)) and not isinstance(direction, bool):
        return direction < 0
    return False


def _junction_lookup(
    junctions: list[dict[str, Any]],
) -> tuple[dict[str, str], dict[int, str]]:
    """Build ``name → key`` and ``uid → key`` maps (key = name or str uid)."""
    name_to_key: dict[str, str] = {}
    uid_to_key: dict[int, str] = {}
    for j in junctions:
        name = j.get("name")
        uid = j.get("uid")
        key = name if isinstance(name, str) else str(uid)
        if isinstance(name, str):
            name_to_key[name] = key
        if isinstance(uid, int) and not isinstance(uid, bool):
            uid_to_key[uid] = key
    return name_to_key, uid_to_key


def _ww_peak(waterway: dict[str, Any], ww_peaks: dict[int, float]) -> float | None:
    """Resolved parquet-peak ``fmax`` for *waterway* keyed by its uid."""
    uid = waterway.get("uid")
    if isinstance(uid, int) and not isinstance(uid, bool):
        return ww_peaks.get(uid)
    return None


def _piecewise_seepage(segments: list[dict[str, Any]], volume: float) -> float | None:
    """Filtration rate [m³/s] at *volume* over the piecewise-linear envelope.

    Each segment is ``{volume, slope, constant}`` in INTERCEPT form —
    ``seepage = constant_i + slope_i · V`` — selected by the greatest
    breakpoint ``volume`` ≤ *V* (the segments are continuous at the
    breakpoints, mirroring ``source/reservoir_seepage_lp.cpp``:
    ``q_filt = constant + slope · efin``).
    """
    chosen: dict[str, Any] | None = None
    for seg in sorted(segments, key=lambda s: s.get("volume", 0.0)):
        bp = seg.get("volume")
        if not isinstance(bp, (int, float)) or isinstance(bp, bool):
            continue
        if chosen is None or bp <= volume:
            chosen = seg
    if chosen is None:
        return None
    const = chosen.get("constant", 0.0)
    slope = chosen.get("slope", 0.0)
    if not isinstance(const, (int, float)) or not isinstance(slope, (int, float)):
        return None
    return float(const) + float(slope) * float(volume)


def _seepage_eval_volume(
    reservoir: dict[str, Any] | None, segments: list[dict[str, Any]]
) -> float:
    """Volume [hm³] at which to evaluate the filtration OUTFLOW cap.

    Uses the reservoir's ``emax`` when it is a concrete number (max pool →
    max seepage); otherwise (``emax`` is a parquet-ref string) falls back to
    the largest segment breakpoint, a conservative high-volume proxy.
    """
    if reservoir is not None:
        emax = reservoir.get("emax")
        if isinstance(emax, (int, float)) and not isinstance(emax, bool):
            return float(emax)
    bps: list[float] = [
        float(vol)
        for s in segments
        if isinstance((vol := s.get("volume")), (int, float))
        and not isinstance(vol, bool)
    ]
    return max(bps) if bps else 0.0


def resolve_reservoir_seepage_caps(system: dict[str, Any]) -> dict[Any, float]:
    """Filtration outflow capacity [m³/s] per seepage waterway (uid + name).

    plp2gtopt models reservoir filtration as a ``reservoir_seepage`` element
    (piecewise ``constant + slope·V``) whose referenced ``filt_*`` waterway
    carries NO numeric ``fmax`` — so the topology estimator would otherwise
    zero the (often large, e.g. Lago Laja ≈ 50 m³/s) filtration outflow.
    PLEXOS already models the same path as a plain waterway ``fmax`` (handled
    by :func:`_numeric_cap`); this closes the gap for the plp dialect.

    Returns ``{waterway_uid: cap, waterway_name: cap}`` evaluated at each
    reservoir's max operating volume; empty when there are no seepage
    elements.
    """
    seepages = system.get("reservoir_seepage_array", []) or []
    if not seepages:
        return {}
    reservoirs = {r.get("name"): r for r in system.get("reservoir_array", []) or []}
    ww_by_name = {w.get("name"): w for w in system.get("waterway_array", []) or []}
    out: dict[Any, float] = {}
    for sp in seepages:
        segs = sp.get("segments") or []
        if not segs:
            continue
        reservoir = reservoirs.get(sp.get("reservoir"))
        rate = _piecewise_seepage(segs, _seepage_eval_volume(reservoir, segs))
        if rate is None or not math.isfinite(rate) or rate <= 0.0:
            continue
        wref = sp.get("waterway")
        w = ww_by_name.get(wref) if isinstance(wref, str) else None
        uid = w.get("uid") if w else (wref if isinstance(wref, int) else None)
        if isinstance(uid, int) and not isinstance(uid, bool):
            out[uid] = rate
        if isinstance(wref, str):
            out[wref] = rate
    return out


def _seepage_cap(
    waterway: dict[str, Any], seepage_caps: dict[Any, float]
) -> float | None:
    """Filtration cap for *waterway* (matched by uid, then name), else None."""
    if not seepage_caps:
        return None
    uid = waterway.get("uid")
    if isinstance(uid, int) and not isinstance(uid, bool) and uid in seepage_caps:
        return seepage_caps[uid]
    nm = waterway.get("name")
    return seepage_caps.get(nm) if isinstance(nm, str) else None


def _turbine_penstock_uids(
    turbines: list[dict[str, Any]],
    ww_by_name: dict[str, dict[str, Any]],
    ww_by_uid: dict[int, dict[str, Any]],
) -> set[int]:
    """Uids of waterways a turbine draws through (its ``_gen_`` penstock).

    In the plp dialect a turbine references a waterway that carries its
    flow; that SAME physical penstock is also counted via the turbine's
    ``pmax/PF`` maxflow.  Returning these uids lets the conduit sum exclude
    the waterway copy so the penstock is counted once (e.g. PANGUE: keep the
    562 m³/s turbine flow, drop the duplicate 562 m³/s ``PANGUE_gen`` arc).
    PLEXOS turbines carry built-in junctions (no waterway ref) → empty set.
    """
    out: set[int] = set()
    for t in turbines:
        ref = t.get("waterway")
        if ref is None:
            continue
        w = ww_by_name.get(ref) if isinstance(ref, str) else ww_by_uid.get(ref)
        if w is None:
            continue
        uid = w.get("uid")
        if isinstance(uid, int) and not isinstance(uid, bool):
            out.add(uid)
    return out


def _turbine_penstock_fmax(
    turbine: dict[str, Any],
    ww_by_name: dict[str, dict[str, Any]],
    ww_by_uid: dict[int, dict[str, Any]],
    ww_peaks: dict[int, float],
) -> float | None:
    """Hydraulic ``fmax`` [m³/s] of the turbine's penstock waterway, or None.

    Used to clamp the turbine's ``pmax/PF`` maxflow to the SERIES bottleneck:
    a turbine cannot pass more water than its penstock allows.  ``pmax/PF``
    uses the worst-case (lowest-head) production factor and so over-states
    the physical flow when the penstock is the binding limit.
    """
    ref = turbine.get("waterway")
    if ref is None:
        return None
    w = ww_by_name.get(ref) if isinstance(ref, str) else ww_by_uid.get(ref)
    if w is None:
        return None
    cap = _numeric_cap(w.get("fmax"))
    if cap is None:
        cap = _ww_peak(w, ww_peaks)
    return cap


def _reservoir_drain_enabled(reservoir: dict[str, Any]) -> bool:
    """True when the reservoir's internal spill (drain) column is ACTIVE.

    "Has a spillway" is a RESERVOIR property — the storage drain column
    parameterised by ``spillway_cost`` / ``spillway_capacity`` — NOT the
    junction ``drain`` flag (which, in PLP-derived data, mostly marks
    out-of-basin ocean sinks).  Mirrors the C++ gate
    (``storage_lp.hpp:593``):

        drain_enabled = spillway_cost.has_value()
                        && spillway_capacity.value_or(1.0) > 0

    A drained reservoir can spill OUT of the basin, so its release is NOT
    limited to the downstream conduits — it can pass a flood on top of
    them.  ELTORO (no ``spillway_cost``) is the un-drained sentinel.
    """
    if reservoir.get("spillway_cost") is None:
        return False
    cap = reservoir.get("spillway_capacity")
    return not (isinstance(cap, (int, float)) and cap <= 0.0)


def estimate_reservoir_flow_bounds(  # noqa: PLR0914,PLR0913  pylint: disable=too-many-locals
    system: dict[str, Any],
    *,
    inflow_peaks: dict[str, float],
    turbine_maxflow: dict[str, float],
    ww_peaks: dict[int, float] | None = None,
    seepage_caps: dict[Any, float] | None = None,
    extra_turbines: list[dict[str, Any]] | None = None,
    default_accept: float = 9000.0,
    default_release: float = 6000.0,
) -> dict[str, tuple[float | None, float | None]]:
    """Pure topology core: mutate reservoirs with estimated ``fmin``/``fmax``.

    *inflow_peaks* maps a junction key → summed peak natural inflow there.
    *turbine_maxflow* maps a turbine key (:func:`_turbine_maxflow_key`) →
    its max flow [m³/s].

    Sets ``reservoir["fmax"] = round(max_release * margin)`` and
    ``reservoir["fmin"] = -round(max_inflow * margin)`` ONLY when the
    corresponding field is absent / sentinel and the estimate is finite &
    strictly positive.  Appends a short note to ``description``.

    Returns ``{reservoir_name: (fmin_set_or_None, fmax_set_or_None)}`` —
    the value applied, or ``None`` when the field was left untouched.
    *default_accept* / *default_release* mirror the C++ fallbacks and are
    used only for the report / never written (kept for caller parity).
    """
    reservoirs = system.get("reservoir_array", []) or []
    waterways = system.get("waterway_array", []) or []
    # ``extra_turbines`` are physical units present in the basin but absent
    # from the dispatch ``turbine_array`` (e.g. PLEXOS drops maintenance-
    # offline units to avoid a free-drain LP artefact).  They are counted
    # for the STRUCTURAL extraction bound only — the dispatch model is
    # untouched — so every physical unit's nameplate sizes the reservoir.
    turbines = list(system.get("turbine_array", []) or []) + list(extra_turbines or [])
    junctions = system.get("junction_array", []) or []
    ww_peaks = ww_peaks or {}

    name_to_key, uid_to_key = _junction_lookup(junctions)
    junction_by_key: dict[str, dict[str, Any]] = {}
    for j in junctions:
        nm = j.get("name")
        key = nm if isinstance(nm, str) else str(j.get("uid"))
        junction_by_key[key] = j
    ww_by_name, ww_by_uid = _build_index_maps(waterways)

    def jkey(ref: Any) -> str | None:
        k = _junction_key(ref, name_to_key, uid_to_key)
        if k is None and isinstance(ref, str):
            return ref
        return k

    # ---- per-junction CONDUIT-out capacity (waterways + turbines) --------
    # This deliberately EXCLUDES drains: an unbounded spillway spills OUT
    # of the system (to a sink), so it must NOT inflate the finite
    # super-source edges that feed downstream reservoirs.  Drain capacity
    # is folded into a reservoir's own ``fmax`` separately.
    seepage_caps = seepage_caps or {}
    # Waterways that are a turbine's penstock are counted via the turbine
    # maxflow below — exclude them here so the same path is not double-counted.
    penstock_uids = _turbine_penstock_uids(turbines, ww_by_name, ww_by_uid)
    conduit_out: dict[str, float] = {}
    for w in waterways:
        if w.get("uid") in penstock_uids:
            continue
        ja = jkey(w.get("junction_a"))
        cap = _numeric_cap(w.get("fmax"))
        if cap is None:
            cap = _ww_peak(w, ww_peaks)
        # A ``filt_*`` waterway carries no numeric fmax — its capacity is the
        # reservoir filtration (seepage) rate, an out-of-pool outflow.
        if cap is None:
            cap = _seepage_cap(w, seepage_caps)
        if ja is not None and cap is not None and cap > 0.0:
            conduit_out[ja] = conduit_out.get(ja, 0.0) + cap
    # directed turbine edges (also used by the max-flow graph below)
    turbine_edges: list[tuple[str, str | None, float]] = []
    for t in turbines:
        cap = turbine_maxflow.get(_turbine_maxflow_key(t))
        if cap is None or cap <= 0.0:
            continue
        # Series bottleneck: clamp pmax/PF to the penstock's hydraulic fmax.
        pen_fmax = _turbine_penstock_fmax(t, ww_by_name, ww_by_uid, ww_peaks)
        if pen_fmax is not None and pen_fmax > 0.0:
            cap = min(cap, pen_fmax)
        ja_ref, jb_ref = _turbine_junctions(t, ww_by_name, ww_by_uid)
        ja = jkey(ja_ref)
        jb = jkey(jb_ref)
        if ja is None:
            continue
        conduit_out[ja] = conduit_out.get(ja, 0.0) + cap
        turbine_edges.append((ja, jb, cap))

    # ---- uncapped extraction outlet → fold downstream conveyance ---------
    # An UNBOUNDED outlet (no numeric fmax / parquet peak / seepage cap) that
    # drains a reservoir junction to a NON-reservoir junction is an uncapped
    # extraction arc (e.g. PLEXOS ``Ext_Maule``: L_Maule → LA_MINA, source
    # ``Max Flow = 1e30``).  ``conduit_out`` would otherwise ignore it,
    # under-sizing the reservoir's release.  Add the downstream junction's
    # onward conveyance so the bound reflects what can physically leave.
    # EXCLUDED — never inflate these:
    #   * spills to a sink (``junction_b`` is None) — water leaves the system;
    #   * spills to a downstream RESERVOIR — already handled by the cascade
    #     accept side (folding here would double-count).
    # Reservoirs that spill through the drain column (no graph edge) or via a
    # numeric/seepage waterway never trigger this, so the per-storage-drain
    # dialects (plexos2gtopt, the forward-ported plp2gtopt) are unaffected.
    reservoir_junctions = {
        jkey(r.get("junction"))
        for r in reservoirs
        if jkey(r.get("junction")) is not None
    }
    for w in waterways:
        if w.get("uid") in penstock_uids:
            continue
        if (
            _numeric_cap(w.get("fmax")) is not None
            or _ww_peak(w, ww_peaks) is not None
            or _seepage_cap(w, seepage_caps) is not None
        ):
            continue
        ja = jkey(w.get("junction_a"))
        jb = jkey(w.get("junction_b"))
        if ja is None or jb is None:
            continue
        if ja not in reservoir_junctions or jb in reservoir_junctions:
            continue
        onward = conduit_out.get(jb, 0.0)
        if onward > 0.0:
            conduit_out[ja] = conduit_out.get(ja, 0.0) + onward

    # ---- reservoir-junction set + per-reservoir release ------------------
    # ``reservoir_release`` (used as super-source feed into downstream) is
    # the FINITE conduit-out only.  A reservoir's own ``fmax`` adds any
    # explicit finite drain; an unbounded drain marks it release-unbounded
    # so ``fmax`` is left unset.
    reservoir_junction: dict[int, str] = {}
    reservoir_release: dict[int, float] = {}  # conduit-only out (cascade feed)
    reservoir_conduit: dict[int, float] = {}
    for idx, r in enumerate(reservoirs):
        rj = jkey(r.get("junction"))
        if rj is None:
            continue
        reservoir_junction[idx] = rj
        conduit = conduit_out.get(rj, 0.0)
        # The cascade-feed edge (__SRC__→J) is the CONDUIT capacity only —
        # the reservoir's internal drain spills OUT of the basin, never
        # toward a downstream reservoir, so it must not inflate cascade flow.
        reservoir_release[idx] = conduit
        reservoir_conduit[idx] = conduit

    # ---- max-flow base graph (capacities; per-R source edge tweaked) -----
    base_graph, src_reservoir_caps = _build_maxflow_graph(
        waterways=waterways,
        turbine_edges=turbine_edges,
        inflow_peaks=inflow_peaks,
        reservoir_junction=reservoir_junction,
        reservoir_release=reservoir_release,
        jkey=jkey,
        ww_peaks=ww_peaks,
        seepage_caps=seepage_caps,
        penstock_uids=penstock_uids,
    )

    report: dict[str, tuple[float | None, float | None]] = {}
    for idx, r in enumerate(reservoirs):
        name = r.get("name", f"reservoir#{idx}")
        rj = reservoir_junction.get(idx)
        if rj is None:
            report[name] = (None, None)
            continue

        conduit = reservoir_conduit.get(idx, 0.0)
        max_inflow = _reservoir_max_inflow(
            base_graph, src_reservoir_caps, target=rj, this_reservoir_idx=idx
        )

        notes: list[str] = []
        fmax_set: float | None = None
        fmin_set: float | None = None

        # MAX EXTRACTION is a PHYSICAL release limit — the sum of the
        # reservoir's downstream-carry capacities (turbines + waterways +
        # filtration/seepage), NOT affluent (inflow) data.  It must NOT
        # depend on how much water arrives, otherwise the same physical
        # reservoir gets wildly different bounds between dialects whose
        # inflow series differ (e.g. ELTORO: plp 352 vs PLEXOS 18.6 m³/s).
        # The spillway/drain is a SEPARATE LP column (bounded by
        # ``spillway_capacity`` in C++), so it is deliberately excluded here.
        max_release = conduit  # turbines + downstream waterways + seepage
        if (
            _is_absent_bound(r.get("fmax"))
            and math.isfinite(max_release)
            and max_release > 0.0
        ):
            fmax_set = float(round(max_release * SAFETY_MARGIN))
            r["fmax"] = fmax_set
            notes.append(f"fmax={fmax_set:g} (physical conduit ×{SAFETY_MARGIN:g})")
        if (
            _is_absent_bound(r.get("fmin"))
            and math.isfinite(max_inflow)
            and max_inflow > 0.0
        ):
            fmin_set = -float(round(max_inflow * SAFETY_MARGIN))
            r["fmin"] = fmin_set
            notes.append(f"fmin={fmin_set:g} (max-flow accept cap ×{SAFETY_MARGIN:g})")

        if notes:
            note = "reservoir-flow estimate: " + ", ".join(notes)
            existing = r.get("description")
            r["description"] = f"{existing}; {note}" if existing else note

        report[name] = (fmin_set, fmax_set)
    return report


def _build_maxflow_graph(  # noqa: PLR0913
    *,
    waterways: list[dict[str, Any]],
    turbine_edges: list[tuple[str, str | None, float]],
    inflow_peaks: dict[str, float],
    reservoir_junction: dict[int, str],
    reservoir_release: dict[int, float],
    jkey: Any,
    ww_peaks: dict[int, float] | None = None,
    seepage_caps: dict[Any, float] | None = None,
    penstock_uids: set[int] | None = None,
) -> tuple[Any, dict[int, tuple[str, float]]]:
    """Build the capacitated DiGraph + per-reservoir source-edge spec.

    Returns ``(graph, src_reservoir_caps)`` where ``src_reservoir_caps``
    maps reservoir index → ``(junction_key, release_cap)`` for the
    ``__SRC__ → junction`` reservoir-release edge that must be *removed*
    when that reservoir is the max-flow target.
    """
    import networkx as nx  # noqa: PLC0415  pylint: disable=import-outside-toplevel

    peaks = ww_peaks or {}
    seepage_caps = seepage_caps or {}
    penstock_uids = penstock_uids or set()
    graph = nx.DiGraph()

    for w in waterways:
        # Penstock waterways are represented by the turbine edge below.
        if w.get("uid") in penstock_uids:
            continue
        ja = jkey(w.get("junction_a"))
        jb = jkey(w.get("junction_b"))
        cap = _numeric_cap(w.get("fmax"))
        if cap is None:
            cap = _ww_peak(w, peaks)
        if cap is None:
            cap = _seepage_cap(w, seepage_caps)
        if ja is None or jb is None or cap is None or cap <= 0.0:
            continue
        _add_capacity(graph, ja, jb, cap)

    for ja, jb, cap in turbine_edges:
        target = jb if jb is not None else _SINK
        _add_capacity(graph, ja, target, cap)

    # Natural-inflow source edges.
    for jkey_name, peak in inflow_peaks.items():
        if peak > 0.0:
            _add_capacity(graph, _SRC, jkey_name, peak)

    # Reservoir-release source edges: an upstream reservoir can dump up to
    # its own release capacity into its junction.  Tracked so the target
    # reservoir's own edge is removed at query time.
    src_reservoir_caps: dict[int, tuple[str, float]] = {}
    for idx, rj in reservoir_junction.items():
        cap = reservoir_release.get(idx, 0.0)
        if cap > 0.0:
            src_reservoir_caps[idx] = (rj, cap)
            _add_capacity(graph, _SRC, rj, cap)
    return graph, src_reservoir_caps


def _add_capacity(graph: Any, u: str, v: str, cap: float) -> None:
    """Add *cap* to the parallel-edge capacity of ``u → v`` (accumulating)."""
    if graph.has_edge(u, v):
        graph[u][v]["capacity"] += cap
    else:
        graph.add_edge(u, v, capacity=cap)


def _reservoir_max_inflow(
    base_graph: Any,
    src_reservoir_caps: dict[int, tuple[str, float]],
    *,
    target: str,
    this_reservoir_idx: int,
) -> float:
    """Max water that can ARRIVE at *target* (R not feeding itself).

    Temporarily removes R's own ``__SRC__ → junction`` reservoir-release
    edge (so R is not its own source) and any *other* reservoirs that sit
    at the same junction, runs ``maximum_flow_value``, then restores.
    """
    import networkx as nx  # noqa: PLC0415  pylint: disable=import-outside-toplevel

    if target not in base_graph:
        return 0.0

    # Remove the reservoir-release source contribution(s) at *target* that
    # belong to this reservoir, leaving natural inflow + other-reservoir
    # contributions at OTHER junctions intact.
    own = src_reservoir_caps.get(this_reservoir_idx)
    removed_cap = 0.0
    if own is not None and own[0] == target and base_graph.has_edge(_SRC, target):
        # The __SRC__→target edge capacity bundles this reservoir's release
        # plus natural inflow plus any co-located reservoir.  Subtract only
        # this reservoir's release so the rest (a true upstream supply)
        # remains.  If that empties the edge, drop it.
        removed_cap = own[1]
        base_graph[_SRC][target]["capacity"] -= removed_cap
        dropped = False
        if base_graph[_SRC][target]["capacity"] <= 0.0:
            base_graph.remove_edge(_SRC, target)
            dropped = True
        try:
            value = nx.maximum_flow_value(base_graph, _SRC, target)
        finally:
            if dropped:
                base_graph.add_edge(_SRC, target, capacity=removed_cap)
            else:
                base_graph[_SRC][target]["capacity"] += removed_cap
        return float(value)

    return float(nx.maximum_flow_value(base_graph, _SRC, target))


def apply_reservoir_flow_estimates(
    planning: dict[str, Any],
    *,
    input_dir: Path | None,
    generator_capacities: dict[Any, float] | None = None,
    extra_turbines: list[dict[str, Any]] | None = None,
    default_accept: float = 9000.0,
    default_release: float = 6000.0,
) -> dict[str, tuple[float | None, float | None]]:
    """Top-level entry: resolve peaks + turbine caps + run the core.

    This is what the converters call after the planning ``system`` is
    fully assembled (and, for plp2gtopt, after the discharge parquet has
    been written so the string-ref peaks resolve).  Mutates the reservoirs
    in ``planning["system"]`` in place and returns the per-reservoir
    report from :func:`estimate_reservoir_flow_bounds`.

    *generator_capacities* optionally overrides each generator's dispatch
    ``pmax`` with its physical NAMEPLATE (all units) for the turbine-maxflow
    used to size the reservoir bound — see :func:`compute_turbine_maxflow`.
    plexos2gtopt passes this so a maintenance-offline unit (dispatch
    ``pmax = 0``) still contributes its nameplate to the structural bound.
    """
    system = planning.get("system")
    if not isinstance(system, dict):
        return {}
    if not (system.get("reservoir_array") or []):
        return {}

    inflow_peaks = resolve_inflow_peaks(system, input_dir=input_dir)
    generator_peaks = resolve_generator_pmax_peaks(system, input_dir=input_dir)
    ww_peaks = resolve_waterway_fmax_peaks(system, input_dir=input_dir)
    seepage_caps = resolve_reservoir_seepage_caps(system)
    min_production_factors = resolve_min_production_factors(system)
    turbine_maxflow = compute_turbine_maxflow(
        system,
        generator_peaks=generator_peaks,
        min_production_factors=min_production_factors,
        generator_capacities=generator_capacities,
        extra_turbines=extra_turbines,
    )
    report = estimate_reservoir_flow_bounds(
        system,
        inflow_peaks=inflow_peaks,
        turbine_maxflow=turbine_maxflow,
        ww_peaks=ww_peaks,
        seepage_caps=seepage_caps,
        extra_turbines=extra_turbines,
        default_accept=default_accept,
        default_release=default_release,
    )
    n_set = sum(
        1 for fmin, fmax in report.values() if fmin is not None or fmax is not None
    )
    logger.info(
        "reservoir_flow: estimated bounds for %d/%d reservoir(s)",
        n_set,
        len(report),
    )
    return report


def widen_extraction_bounds_symmetric(
    planning: dict[str, Any], *, factor: float = 2.0
) -> dict[str, tuple[float | None, float | None]]:
    """Replace each reservoir's extraction box with a SYMMETRIC ``±factor·e``.

    ``e = max(|fmin|, |fmax|)`` of the estimator-computed extraction bounds
    (run :func:`apply_reservoir_flow_estimates` first).  Each reservoir's
    ``fmin``/``fmax`` is overwritten with ``-factor·e`` / ``+factor·e`` — a
    loose-but-finite symmetric box keyed off the estimator's magnitude
    (avoids free columns while not cutting feasible operation, and is
    sign-symmetric for solver conditioning).  Reservoirs whose estimator
    bounds are both absent are left untouched.  Returns
    ``{reservoir_name: (fmin, fmax)}`` for the values applied.
    """
    system = planning.get("system")
    if not isinstance(system, dict):
        return {}
    out: dict[str, tuple[float | None, float | None]] = {}
    for r in system.get("reservoir_array", []) or []:
        name = r.get("name", "")
        mags = [
            abs(float(v))
            for v in (r.get("fmin"), r.get("fmax"))
            if isinstance(v, (int, float)) and not isinstance(v, bool)
        ]
        if not mags:
            out[name] = (None, None)
            continue
        e = max(mags)
        r["fmin"] = -factor * e
        r["fmax"] = factor * e
        out[name] = (r["fmin"], r["fmax"])
    return out


def drop_large_reservoir_spillways(
    system: dict[str, Any],
    reservoir_capacities: dict[str, float],
    threshold_hm3: float,
    *,
    protected_waterways: frozenset[str] = frozenset(),
) -> list[str]:
    """Drop the physical ``_ver`` / ``Vert_*`` spillway arc of LARGE reservoirs.

    Shared so both converters can apply the SAME small-reservoir-spillway
    selection on top of the SAME extraction estimator.  Call this BEFORE
    :func:`apply_reservoir_flow_estimates` so the estimate reflects the
    corrected topology (the plexos2gtopt caller already does this).

    A reservoir whose storage capacity is ``>= threshold_hm3`` (Hm³) can buffer
    a wet inflow, so it spills solely via its internal ``spillway_cost`` storage
    drain — its parallel downstream spillway waterway is a redundant escape path
    and is dropped.  Small reservoirs (``< threshold_hm3``, e.g. PANGUE ~72 Hm³,
    which is effectively run-of-river) KEEP the spillway arc so they can both
    cycle water (drain) and shed extra inflow downstream (the "small battery").

    ``threshold_hm3 <= 0`` disables the drop (every reservoir keeps its arc).
    A spillway waterway named in ``protected_waterways`` (e.g. referenced by a
    UserConstraint) is never dropped.  A spillway arc is identified as a
    waterway whose ``junction_a`` is a large reservoir and whose name carries
    the ``Vert_`` / ``_ver`` vertimiento tag.  Returns the names dropped.
    """
    if threshold_hm3 <= 0.0:
        return []
    large = {
        name
        for name, cap in reservoir_capacities.items()
        if isinstance(cap, (int, float))
        and not isinstance(cap, bool)
        and cap >= threshold_hm3
    }
    if not large:
        return []
    kept: list[dict[str, Any]] = []
    dropped: list[str] = []
    for ww in system.get("waterway_array", []) or []:
        name = str(ww.get("name", ""))
        is_vert = name.startswith("Vert_") or "_ver" in name
        if (
            is_vert
            and ww.get("junction_a") in large
            and name not in protected_waterways
        ):
            dropped.append(name)
            continue
        kept.append(ww)
    if dropped:
        system["waterway_array"] = kept
    return dropped

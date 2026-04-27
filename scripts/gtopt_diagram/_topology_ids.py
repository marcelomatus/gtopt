# SPDX-License-Identifier: BSD-3-Clause
"""ID + lookup helpers for :class:`gtopt_diagram.TopologyBuilder`.

Mixin extracted from :mod:`gtopt_diagram.gtopt_diagram`.  Holds the
20+ trivial ``_*id()`` helpers that compose graph-node identifiers
plus the small lookup helpers (``_resolve_field``, ``_find``,
``_find_node_id``, ``_bus_node_id``, ``_gen_kind``).

The mixin assumes the host class provides:

* ``self.sys``           — the parsed ``system`` JSON dict
* ``self._vmap``         — voltage-level bus reduction map
* ``self._bus_node_ids`` — set of bus node IDs visible in the model
* ``self._resolver``     — :class:`FieldSchedResolver` or ``None``
* ``self._turb_refs``    — turbine→generator reference map

These attributes are initialised by ``TopologyBuilder.__init__``.
"""

from __future__ import annotations

from typing import Any

from gtopt_diagram._classify import (
    classify_gen as _classify_gen,
    resolve as _resolve,
    scalar as _scalar,
)
from gtopt_diagram._data_utils import (
    resolve_bus_ref as _resolve_bus_ref,
)


class TopologyIdsMixin:
    """Composable graph-ID + lookup helpers."""

    # Shared instance attributes initialised by ``TopologyBuilder.__init__``.
    sys: dict[str, Any]
    _vmap: dict[str, str]
    _bus_node_ids: set
    _resolver: Any
    _turb_refs: set

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
        return TopologyIdsMixin._make_id("bus", b)

    @staticmethod
    def _gid(g):
        return TopologyIdsMixin._make_id("gen", g)

    @staticmethod
    def _did(d):
        return TopologyIdsMixin._make_id("dem", d)

    @staticmethod
    def _batid(b):
        return TopologyIdsMixin._make_id("bat", b)

    @staticmethod
    def _cid(c):
        return TopologyIdsMixin._make_id("conv", c)

    @staticmethod
    def _jid(j):
        return TopologyIdsMixin._make_id("junc", j)

    @staticmethod
    def _rid(r):
        return TopologyIdsMixin._make_id("res", r)

    @staticmethod
    def _tid(t):
        return TopologyIdsMixin._make_id("turb", t)

    @staticmethod
    def _fid(f):
        return TopologyIdsMixin._make_id("flow", f)

    @staticmethod
    def _filtid(fi):
        return TopologyIdsMixin._make_id("filt", fi)

    @staticmethod
    def _rzid(rz):
        return TopologyIdsMixin._make_id("rzone", rz)

    @staticmethod
    def _rpid(rp):
        return TopologyIdsMixin._make_id("rprov", rp)

    @staticmethod
    def _gpid(gp):
        return TopologyIdsMixin._make_id("gprof", gp)

    @staticmethod
    def _dpid(dp):
        return TopologyIdsMixin._make_id("dprof", dp)

    @staticmethod
    def _vrid(vr):
        return TopologyIdsMixin._make_id("vright", vr)

    @staticmethod
    def _frid(fr):
        return TopologyIdsMixin._make_id("fright", fr)

    @staticmethod
    def _pid(p):
        return TopologyIdsMixin._make_id("pump", p)

    @staticmethod
    def _lngid(lt):
        return TopologyIdsMixin._make_id("lng", lt)

    def _resolve_field(
        self, class_name: str, elem: dict, field: str, fallback: str = "—"
    ) -> str:
        """Resolve a field value, reading from Parquet if it's a file reference.

        Returns a formatted string suitable for display in labels.
        """
        val = elem.get(field)
        if val is None:
            return fallback
        if isinstance(val, (int, float)):
            return f"{val:,.1f}" if isinstance(val, float) else str(val)
        if isinstance(val, str) and self._resolver is not None:
            resolved = self._resolver.resolve(
                class_name, val, elem.get("uid"), fallback=None
            )
            if resolved is not None:
                return f"{resolved:,.1f}"
        if isinstance(val, str):
            return fallback  # unresolved file reference
        return _scalar(val)

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

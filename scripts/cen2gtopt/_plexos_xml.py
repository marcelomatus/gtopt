# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt._plexos_xml`` — minimal PLEXOS XML reader.

Parses just the relations the marginal-emission pipeline cares about
out of CEN's ``DBSEN_PRGDIARIO.xml`` (the PLEXOS object database
shipped inside every ``DATOS{date}.zip`` / ``PID/.../Modelos`` bundle).

We deliberately do **not** load the entire 4 533-object schema —
only the t_object + t_collection + t_membership tables for the
relations we need:

  * **Generator → Fuel** (``collection_id=7`` in CEN's PCP) —
    associates each generator unit with its primary fuel.  Used by
    :mod:`cen2gtopt.pcp_inputs.PcpInputs.units` to populate the
    ``fuel_name`` column without the substring-matching heuristic.

  * **Generator → Node** (``collection_id`` discovered at runtime by
    parent_class_id=2 ∧ child_class_id=22) — gives unit-to-bus
    mapping for nodal LMP attribution.

The XML is large (~67k data rows in PCP) but the relations we need
are small (~1700 generators × ~200 fuels).  Parsing is lazy and
cached — first call costs ~1 s, subsequent calls are dict lookups.
"""

from __future__ import annotations

import logging
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import pandas as pd

_LOG = logging.getLogger("cen2gtopt.plexos_xml")

#: PLEXOS MasterDataSet XML namespace.
NS = "{http://tempuri.org/MasterDataSet.xsd}"

#: Class IDs from CEN's PRGDIARIO schema (verified against
#: ``DBSEN_PRGDIARIO.xml`` shipped with PCP/PID 2026-04-07).
CLASS_GENERATOR = 2
CLASS_FUEL = 4
CLASS_NODE = 22

#: Collection IDs.  Only the Generator-direct ones (parent_class_id=2);
#: there are also Region- and Company-scoped Fuel collections we
#: ignore.
COLLECTION_GENERATOR_FUELS = 7  # Generator → Fuel (primary)
COLLECTION_GENERATOR_START_FUELS = 8  # Generator → Fuel (startup only)


# ---------------------------------------------------------------------------
# Low-level XML parsing
# ---------------------------------------------------------------------------


@dataclass
class PlexosObjects:
    """Parsed view of the PLEXOS XML object database."""

    object_name_by_id: dict[int, str]
    object_class_by_id: dict[int, int]

    def names_of_class(self, class_id: int) -> dict[int, str]:
        """All ``{object_id → name}`` for one class."""
        return {
            oid: name
            for oid, name in self.object_name_by_id.items()
            if self.object_class_by_id.get(oid) == class_id
        }


def _parse_objects(root: ET.Element) -> PlexosObjects:
    name_by_id: dict[int, str] = {}
    class_by_id: dict[int, int] = {}
    for obj in root.findall(f"{NS}t_object"):
        oid = obj.findtext(f"{NS}object_id")
        name = obj.findtext(f"{NS}name")
        cid = obj.findtext(f"{NS}class_id")
        if oid is None or name is None or cid is None:
            continue
        try:
            ioid = int(oid)
        except ValueError:
            continue
        try:
            icid = int(cid)
        except ValueError:
            continue
        name_by_id[ioid] = name
        class_by_id[ioid] = icid
    return PlexosObjects(
        object_name_by_id=name_by_id,
        object_class_by_id=class_by_id,
    )


def _parse_memberships(
    root: ET.Element,
    *,
    collection_id: int,
) -> list[tuple[int, int]]:
    """Return ``[(parent_object_id, child_object_id), …]`` for the
    given collection."""
    out: list[tuple[int, int]] = []
    for mem in root.findall(f"{NS}t_membership"):
        cid = mem.findtext(f"{NS}collection_id")
        if cid is None or int(cid) != collection_id:
            continue
        po = mem.findtext(f"{NS}parent_object_id")
        co = mem.findtext(f"{NS}child_object_id")
        if po is None or co is None:
            continue
        try:
            out.append((int(po), int(co)))
        except ValueError:
            continue
    return out


# ---------------------------------------------------------------------------
# High-level helpers
# ---------------------------------------------------------------------------


@lru_cache(maxsize=8)
def _load_root(xml_path: str) -> ET.Element:
    """Memoised XML parse.  ``xml_path`` is stringified for cache
    hashability."""
    _LOG.info("parsing %s", xml_path)
    tree = ET.parse(xml_path)
    return tree.getroot()


def parse_unit_fuel_map(xml_path: Path | str) -> dict[str, str]:
    """Return ``{generator_name → primary_fuel_name}`` from a PCP/PID
    PLEXOS XML.

    Uses the **primary Fuels** collection (``collection_id=7``) only.
    Start Fuels (collection 8) are excluded — they're cold-start
    auxiliaries, not the main combustion fuel.
    """
    xml_path = str(xml_path)
    root = _load_root(xml_path)
    objects = _parse_objects(root)
    gen_names = objects.names_of_class(CLASS_GENERATOR)
    fuel_names = objects.names_of_class(CLASS_FUEL)

    pairs = _parse_memberships(
        root,
        collection_id=COLLECTION_GENERATOR_FUELS,
    )
    out: dict[str, str] = {}
    for gen_oid, fuel_oid in pairs:
        gname = gen_names.get(gen_oid)
        fname = fuel_names.get(fuel_oid)
        if gname is None or fname is None:
            continue
        # Multiple fuels per generator: prefer the first listed
        # (PLEXOS order = priority).
        out.setdefault(gname, fname)
    return out


def parse_unit_node_map(xml_path: Path | str) -> dict[str, str]:
    """Return ``{generator_name → node_name}`` (unit → bus mapping).

    Auto-discovers the Generator-Node collection by walking
    ``t_collection`` for ``parent_class_id=2 ∧ child_class_id=22``.
    """
    xml_path = str(xml_path)
    root = _load_root(xml_path)

    # Discover the Generator → Node collection
    gn_collection_id: int | None = None
    for col in root.findall(f"{NS}t_collection"):
        try:
            pcid = int(col.findtext(f"{NS}parent_class_id") or 0)
            ccid = int(col.findtext(f"{NS}child_class_id") or 0)
        except ValueError:
            continue
        if pcid == CLASS_GENERATOR and ccid == CLASS_NODE:
            gn_collection_id = int(col.findtext(f"{NS}collection_id") or 0)
            break
    if gn_collection_id is None:
        return {}

    objects = _parse_objects(root)
    gen_names = objects.names_of_class(CLASS_GENERATOR)
    node_names = objects.names_of_class(CLASS_NODE)

    pairs = _parse_memberships(root, collection_id=gn_collection_id)
    out: dict[str, str] = {}
    for gen_oid, node_oid in pairs:
        gname = gen_names.get(gen_oid)
        nname = node_names.get(node_oid)
        if gname is None or nname is None:
            continue
        out.setdefault(gname, nname)
    return out


def summary(xml_path: Path | str) -> pd.DataFrame:
    """One-row summary DataFrame of the schema (for diagnostics)."""
    xml_path = str(xml_path)
    root = _load_root(xml_path)
    objects = _parse_objects(root)
    return pd.DataFrame(
        [
            {
                "n_generators": len(objects.names_of_class(CLASS_GENERATOR)),
                "n_fuels": len(objects.names_of_class(CLASS_FUEL)),
                "n_nodes": len(objects.names_of_class(CLASS_NODE)),
                "n_unit_fuel_pairs": len(parse_unit_fuel_map(xml_path)),
                "n_unit_node_pairs": len(parse_unit_node_map(xml_path)),
            }
        ]
    )


#: Generator-class properties on the ``input`` side of the PLEXOS XML
#: that the marginal-emission picker can consume.  Names match the
#: PLEXOS canonical property names (case-sensitive).
INPUT_PROPERTIES_OF_INTEREST: tuple[str, ...] = (
    "Max Capacity",  # MW — pmax  (preferred over CEN pot_neta_efectiva)
    "Min Stable Level",  # MW — pmin  (preferred over CEN max(tech, env, freq))
    "Heat Rate",  # kJ/kWh — single-block heat rate when present
    "Heat Rate Base",  # kJ — fixed-load heat consumption
    "Heat Rate Incr",  # kJ/kWh — marginal heat rate (rare in CEN exports)
    "Min Up Time",  # h
    "Min Down Time",  # h
    "Start Cost",  # $
    "VO&M Charge",  # $/MWh
    "Fuel Price",  # $/MMBtu (or fuel-specific unit)
)


@lru_cache(maxsize=4)
def parse_generator_input_properties(
    xml_path: str,
    properties: tuple[str, ...] = INPUT_PROPERTIES_OF_INTEREST,
) -> dict[str, dict[str, float]]:
    """Extract per-generator input properties from a PLEXOS PID/PCP XML.

    Returns ``{generator_name → {property_name → numeric value}}``.

    PLEXOS stores input data in ``t_data`` (membership_id, property_id,
    value).  A row's generator is resolved by following:

        membership_id → t_membership.child_object_id → t_object.name

    Multi-band properties (heat-rate curves with breakpoints) are
    summarised to a single value here; full per-band recovery is a
    separate concern (see ``parse_generator_band_data``).

    Properties absent for a unit simply don't appear in its dict.
    Non-numeric values are silently skipped.

    The result is cached (LRU=4) so re-querying a parsed XML costs
    a dict lookup rather than re-parsing the 36 MB document.
    """
    root = _load_root(xml_path)

    # 1. Resolve property names → property_ids (Generator collection_id=1).
    name_to_pid: dict[str, int] = {}
    pid_to_name: dict[int, str] = {}
    for p in root.findall(f"{NS}t_property"):
        name = p.findtext(f"{NS}name")
        cid = p.findtext(f"{NS}collection_id")
        if not name or cid != "1" or name not in properties:
            continue
        try:
            pid = int(p.findtext(f"{NS}property_id") or 0)
        except ValueError:
            continue
        name_to_pid[name] = pid
        pid_to_name[pid] = name
    if not name_to_pid:
        return {}

    # 2. Generator object_id → name.
    objects = _parse_objects(root)
    gen_names = objects.names_of_class(CLASS_GENERATOR)

    # 3. membership_id → child_object_id (Generator-collection membership).
    mem_to_obj: dict[int, int] = {}
    for m in root.findall(f"{NS}t_membership"):
        cid = m.findtext(f"{NS}collection_id")
        if cid != "1":
            continue
        try:
            mid = int(m.findtext(f"{NS}membership_id") or 0)
            cobj = int(m.findtext(f"{NS}child_object_id") or 0)
        except ValueError:
            continue
        mem_to_obj[mid] = cobj

    # 4. Walk t_data rows and project property values per generator.
    out: dict[str, dict[str, float]] = {}
    for d in root.findall(f"{NS}t_data"):
        try:
            pid = int(d.findtext(f"{NS}property_id") or 0)
        except ValueError:
            continue
        prop_name = pid_to_name.get(pid)
        if prop_name is None:
            continue
        try:
            mid = int(d.findtext(f"{NS}membership_id") or 0)
        except ValueError:
            continue
        gen_name = gen_names.get(mem_to_obj.get(mid, -1))
        if not gen_name:
            continue
        try:
            val = float(d.findtext(f"{NS}value") or "")
        except ValueError:
            continue
        out.setdefault(gen_name, {})[prop_name] = val
    return out


__all__ = [
    "PlexosObjects",
    "INPUT_PROPERTIES_OF_INTEREST",
    "parse_unit_fuel_map",
    "parse_unit_node_map",
    "parse_generator_input_properties",
    "summary",
]

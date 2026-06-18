"""Minimal PLEXOS MasterDataSet XML reader.

A generalisation of ``cen2gtopt._plexos_xml`` (which currently extracts
only generator→fuel + generator→node maps for marginal-emission
analytics). This module exposes the full t_class / t_object /
t_collection / t_membership / t_property / t_data surface that the
converter needs, with an LRU-cached XML parse so re-querying the same
36 MB file is essentially free.

Design choices:

* Pure ``xml.etree.ElementTree`` — no external dep. The XML is large
  but flat, so SAX-style streaming is unnecessary; a single
  ``ET.parse`` followed by ``findall`` per table costs ~1 s and
  ~250 MB peak.
* Numeric IDs auto-discovered. The 96-class CEN PCP schema reshuffles
  ``class_id`` values between PLEXOS releases, so we look up classes
  by *name* (``Generator``, ``Node``, ``Fuel``, ``Line``, …) and let
  the user override on the rare conflict.
* The :class:`PlexosDb` view returned is a thin, immutable wrapper —
  no caching of derived projections at this layer; downstream parsers
  build whatever indexes they need.
"""

from __future__ import annotations

import logging
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from functools import lru_cache
from pathlib import Path


logger = logging.getLogger(__name__)


#: PLEXOS MasterDataSet XML namespace.
NS = "{http://tempuri.org/MasterDataSet.xsd}"


@dataclass
class PlexosObject:
    """One ``t_object`` row."""

    object_id: int
    class_id: int
    name: str
    #: Optional PLEXOS category id (``t_object.category_id``) — links to a
    #: ``t_category`` row carrying a human-readable category name (e.g.
    #: ``"Solar Farms"``, ``"Wind Farms"``, ``"Hydro Gen Group A"``,
    #: ``"Thermal Gen S. Zone"``).  Used by the converter to classify
    #: generators into the canonical primary-energy taxonomy when the
    #: name suffix alone is insufficient (e.g. CEN hydro names like
    #: ``ABANICO`` / ``ALFALFAL`` carry no fuel suffix).  ``None`` when
    #: the bundle XML omits the field (older PLEXOS schemas).
    category_id: int | None = None


@dataclass
class PlexosCollection:
    """One ``t_collection`` row — relation definition."""

    collection_id: int
    parent_class_id: int
    child_class_id: int
    name: str


@dataclass
class PlexosMembership:
    """One ``t_membership`` row — concrete (parent_obj → child_obj) edge."""

    membership_id: int
    collection_id: int
    parent_object_id: int
    child_object_id: int


@dataclass
class PlexosProperty:
    """One ``t_property`` row — dynamic property definition."""

    property_id: int
    collection_id: int
    name: str


@dataclass
class PlexosDataRow:
    """One ``t_data`` row — numeric property value.

    ``date_from`` / ``date_to`` mirror the optional PLEXOS ``t_date_from`` /
    ``t_date_to`` rows keyed by ``data_id``.  A row carrying a date window
    is a *dated override* that applies only while the simulation timestamp
    falls inside ``[date_from, date_to]``; rows with no window are the
    permanent base value.  See :func:`PlexosDb.horizon_start` and the
    constraint RHS selection in ``parsers.build_user_constraints``.
    """

    data_id: int
    membership_id: int
    property_id: int
    value: float
    period_id: int | None = None
    date_from: datetime | None = None
    date_to: datetime | None = None
    #: Optional PLEXOS timeslice tag (``<t_text class_id=76>`` companion
    #: record), e.g. ``"H16-20"`` (every day hours 16-20), ``"H1-8,H20-24"``
    #: (split shift), or ``"W2-6,H8-21"`` (weekday hours 8-21).  Independent
    #: of ``date_from``/``date_to``: dates scope a value to specific calendar
    #: days; a timeslice scopes it to a recurring hour-of-day / day-of-week
    #: pattern that fires inside whatever date window the row carries (or all
    #: dates, when undated).  See :func:`_parse_timeslice_map` for the source
    #: table and the constraint RHS overlay path in
    #: :func:`parsers.extract_user_constraints`.
    timeslice: str | None = None


@dataclass
class PlexosDb:
    """In-memory view of a parsed ``DBSEN_PRGDIARIO.xml``.

    Tables are stored as plain lists; the helpers below build the
    typical indexes lazily so simple "iterate every object" code
    stays cheap.
    """

    path: Path
    classes_by_id: dict[int, str] = field(default_factory=dict)
    classes_by_name: dict[str, int] = field(default_factory=dict)
    objects: list[PlexosObject] = field(default_factory=list)
    #: ``t_category.category_id → category_name`` lookup — populated
    #: from ``t_category`` rows.  Combined with
    #: :attr:`PlexosObject.category_id` to classify generators into the
    #: canonical primary-energy taxonomy (see
    #: :func:`plexos2gtopt.parsers.primary_energy_of_generator`).
    categories_by_id: dict[int, str] = field(default_factory=dict)
    collections: list[PlexosCollection] = field(default_factory=list)
    memberships: list[PlexosMembership] = field(default_factory=list)
    properties: list[PlexosProperty] = field(default_factory=list)
    data_rows: list[PlexosDataRow] = field(default_factory=list)
    #: ``t_tag`` index — ``{data_id -> [scenario_object_id, …]}``.  PLEXOS
    #: uses ``t_tag`` to mark a data row as scenario-conditional: the row
    #: is active only when one of its tagged objects (typically a Scenario)
    #: is enabled in the Model being run.  Untagged rows are the default.
    #: Critical for resolving dual-row ``Include in ST Schedule`` values
    #: (one untagged ``0``, one Scenario-tagged ``-1``): the ``-1`` wins
    #: only when its Scenario is in the Model's ``Scenarios`` collection.
    tag_for_data: dict[int, list[int]] = field(default_factory=dict)
    #: Simulation horizon start (Horizon "Date From" attribute, OLE-serial
    #: decoded).  ``None`` when the bundle carries no Horizon date.  Used to
    #: resolve dated property overrides (see :class:`PlexosDataRow`).
    horizon_start: datetime | None = None

    # ----- precomputed lookup indexes ---------------------------------------
    #
    # ``data_index[(membership_id, property_id)] -> list[PlexosDataRow]``
    # is built lazily on first call to :meth:`data_for` so the cost of
    # the dict materialisation is paid only when extractors actually
    # need t_data. On the CEN PCP daily bundle (~72k rows) the index
    # takes ~50 ms to build and dwarfs the per-extractor lookup cost
    # we save (Reactance × 317 lines, SOC × 41 batteries, …).
    _data_index: dict[tuple[int, int], list[PlexosDataRow]] | None = field(
        default=None, init=False, repr=False
    )
    # ``sys_membership[(child_class_id, child_object_id)] -> membership_id``
    # for the System→Class collection — same lazy-build pattern.
    _sys_mem_index: dict[tuple[int, int], int] | None = field(
        default=None, init=False, repr=False
    )
    # ``property_index[(collection_id, name)] -> property_id``.
    _prop_index: dict[tuple[int, str], int] | None = field(
        default=None, init=False, repr=False
    )

    # ----- basic accessors ---------------------------------------------------

    def class_id(self, name: str) -> int | None:
        """Return the class_id for ``name`` (case-sensitive); ``None`` if absent."""
        return self.classes_by_name.get(name)

    def objects_of_class(self, name: str) -> list[PlexosObject]:
        """Return all objects whose class name matches ``name``."""
        cid = self.class_id(name)
        if cid is None:
            return []
        return [o for o in self.objects if o.class_id == cid]

    def collection_for(
        self, parent_class: str, child_class: str
    ) -> PlexosCollection | None:
        """Look up a collection by (parent_class_name, child_class_name).

        When several collections match (PLEXOS allows multiple
        Generator→Fuel collections — primary fuels vs start fuels —
        in particular), the lowest collection_id wins to match
        ``cen2gtopt._plexos_xml`` behaviour. Callers needing a specific
        collection should pass the explicit ``collection_id`` instead,
        or use :meth:`collection_for_named` with the collection's PLEXOS
        name (``"Node From"`` vs ``"Node To"``, etc.).
        """
        pcid = self.class_id(parent_class)
        ccid = self.class_id(child_class)
        if pcid is None or ccid is None:
            return None
        matches = [
            c
            for c in self.collections
            if c.parent_class_id == pcid and c.child_class_id == ccid
        ]
        return min(matches, key=lambda c: c.collection_id) if matches else None

    def collection_for_named(
        self, parent_class: str, child_class: str, name: str
    ) -> PlexosCollection | None:
        """Look up a collection by (parent_class, child_class, name).

        PLEXOS distinguishes "Node From" vs "Node To", "Head Storage"
        vs "Tail Storage", "Fuels" vs "Start Fuels" only by the
        collection's ``name``. Use this helper when the directional or
        semantic distinction matters.
        """
        pcid = self.class_id(parent_class)
        ccid = self.class_id(child_class)
        if pcid is None or ccid is None:
            return None
        for c in self.collections:
            if (
                c.parent_class_id == pcid
                and c.child_class_id == ccid
                and c.name == name
            ):
                return c
        return None

    def memberships_of(self, collection_id: int) -> list[PlexosMembership]:
        """Return all memberships in the given collection."""
        return [m for m in self.memberships if m.collection_id == collection_id]

    def parent_to_children(self, collection_id: int) -> dict[int, list[int]]:
        """Index memberships as ``{parent_object_id -> [child_object_id, …]}``.

        Child order preserves the order of appearance in ``t_membership``,
        which matters when a directional collection (e.g. ``Node From``)
        is implicitly single-valued but stored as a list.
        """
        out: dict[int, list[int]] = {}
        for m in self.memberships:
            if m.collection_id == collection_id:
                out.setdefault(m.parent_object_id, []).append(m.child_object_id)
        return out

    def object_by_id(self) -> dict[int, PlexosObject]:
        """Return an ``{object_id -> PlexosObject}`` lookup index."""
        return {o.object_id: o for o in self.objects}

    def property_by_name(self, collection_id: int, name: str) -> int | None:
        """Return ``property_id`` for ``(collection_id, name)`` or ``None``."""
        if self._prop_index is None:
            idx: dict[tuple[int, str], int] = {}
            for p in self.properties:
                idx.setdefault((p.collection_id, p.name), p.property_id)
            self._prop_index = idx
        return self._prop_index.get((collection_id, name))

    def data_for(self, membership_id: int, property_id: int) -> list[PlexosDataRow]:
        """All ``t_data`` rows for ``(membership_id, property_id)``.

        Multi-band properties (heat-rate breakpoints, piecewise loss
        curves) ship several rows per (membership, property). Single-
        valued properties return at most one row.
        """
        if self._data_index is None:
            idx: dict[tuple[int, int], list[PlexosDataRow]] = {}
            for d in self.data_rows:
                idx.setdefault((d.membership_id, d.property_id), []).append(d)
            self._data_index = idx
        return self._data_index.get((membership_id, property_id), [])

    def active_scenario_ids(self, model_name: str | None = None) -> set[int]:
        """Return the set of Scenario ``object_id``s enabled by a Model.

        Args:
            model_name: Name of the Model object (PLEXOS ``Model`` class)
                whose Scenarios collection defines the active scenario
                set.  If ``None`` and the bundle contains exactly one
                Model, that one is used.  If multiple Models exist and
                ``None`` is passed, prefer the canonical CEN PRGdia name
                ``"PRGdia_Full_Definitivo"``; fall back to the first.

        Returns:
            Set of Scenario object_ids that are members of the Model's
            ``Scenarios`` collection.  Empty if no Model is found, the
            Model has no Scenarios collection, or no Scenario class
            exists.

        Used by :func:`parsers.extract_user_constraints` to decide whether
        a Scenario-tagged ``Include in ST Schedule = -1`` row overrides
        the untagged ``0`` default, matching PLEXOS run semantics.
        """
        models = self.objects_of_class("Model")
        if not models:
            return set()
        if model_name is None:
            chosen = next(
                (m for m in models if m.name == "PRGdia_Full_Definitivo"),
                models[0],
            )
        else:
            match = next((m for m in models if m.name == model_name), None)
            if match is None:
                return set()
            chosen = match
        scn_class_id = self.class_id("Scenario")
        if scn_class_id is None:
            return set()
        # Restrict to memberships whose child is a Scenario object —
        # the Model also memberships Horizon / Performance / Report /
        # Stochastic / MT-Schedule / etc., none of which carry tags on
        # data rows.
        scenario_oids = {
            o.object_id for o in self.objects if o.class_id == scn_class_id
        }
        return {
            m.child_object_id
            for m in self.memberships
            if m.parent_object_id == chosen.object_id
            and m.child_object_id in scenario_oids
        }

    def system_membership_id(
        self, child_class: str, child_object_id: int
    ) -> int | None:
        """Return the System→``child_class`` membership_id for ``child_object_id``.

        PLEXOS stores static (non-topology) properties of an object on
        its membership in the auto-generated ``System → <Class>``
        collection (e.g. ``Lines``, ``Generators``, ``Batteries``).
        This helper resolves the membership_id in one shot, so the
        caller can chain into :meth:`data_for`.
        """
        if self._sys_mem_index is None:
            idx: dict[tuple[int, int], int] = {}
            sys_id = self.class_id("System")
            if sys_id is not None:
                sys_collections = {
                    c.collection_id: c.child_class_id
                    for c in self.collections
                    if c.parent_class_id == sys_id
                }
                for m in self.memberships:
                    child_cid = sys_collections.get(m.collection_id)
                    if child_cid is not None:
                        idx[(child_cid, m.child_object_id)] = m.membership_id
            self._sys_mem_index = idx
        cid = self.class_id(child_class)
        if cid is None:
            return None
        return self._sys_mem_index.get((cid, child_object_id))

    def static_property(
        self,
        child_class: str,
        child_object_id: int,
        property_name: str,
        *,
        default: float = 0.0,
        keep_sentinel: bool = False,
    ) -> float:
        """Convenience: look up a single-valued System-collection property.

        Returns ``default`` when the property is undefined for this
        object (the System collection ships the class-wide default,
        which we treat as "no override"). Multi-band rows collapse to
        the lowest ``data_id`` (smallest band index by convention).

        PLEXOS ``±1E+30`` sentinel handling: a magnitude ≥ 1e20 means
        "unbounded / unset" in PLEXOS (the value ``1.0E+30`` is hard-
        coded as the infinity sentinel across virtually every PLEXOS
        property — Max Flow, Min Rating, Max Response, Initial Hours
        Down, Water Value, Offer Quantity, Offer Price, etc.).  Audit
        of DATOS20260422 found 11,542 such rows across 14 distinct
        properties.  Returning a literal 1e+30 would land on the LP
        as a hard bound after equilibration scaling and trip CPLEX
        presolve infeasibility.  Treat as the caller-provided
        ``default`` instead — the caller can then apply property-
        specific semantics (e.g. ``Max Flow`` → no thermal cap).

        Pass ``keep_sentinel=True`` when the caller WANTS to see the
        sentinel verbatim so it can apply a property-specific rule
        (e.g. ``Water Value 1e+30`` → never-drain reservoir).
        """
        coll = self.collection_for("System", child_class)
        if coll is None:
            return default
        prop_id = self.property_by_name(coll.collection_id, property_name)
        if prop_id is None:
            return default
        mid = self.system_membership_id(child_class, child_object_id)
        if mid is None:
            return default
        rows = self.data_for(mid, prop_id)
        if not rows:
            return default
        rows.sort(key=lambda r: r.data_id)
        raw = rows[0].value
        # PLEXOS infinity sentinel: |v| ≥ 1e20 means "unset / unbounded".
        if not keep_sentinel and abs(raw) >= 1.0e20:
            return default
        return raw


# ---------------------------------------------------------------------------
# Low-level parsing helpers
# ---------------------------------------------------------------------------


def _findtext_int(elem: ET.Element, tag: str) -> int | None:
    raw = elem.findtext(f"{NS}{tag}")
    if raw is None:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def _findtext_float(elem: ET.Element, tag: str) -> float | None:
    raw = elem.findtext(f"{NS}{tag}")
    if raw is None or raw == "":
        return None
    try:
        return float(raw)
    except ValueError:
        return None


def _parse_classes(root: ET.Element) -> tuple[dict[int, str], dict[str, int]]:
    by_id: dict[int, str] = {}
    by_name: dict[str, int] = {}
    for elem in root.findall(f"{NS}t_class"):
        cid = _findtext_int(elem, "class_id")
        name = elem.findtext(f"{NS}name")
        if cid is None or name is None:
            continue
        by_id[cid] = name
        # First wins on duplicate names (PLEXOS occasionally ships
        # disabled aliases with the same name).
        by_name.setdefault(name, cid)
    return by_id, by_name


def _parse_objects(root: ET.Element) -> list[PlexosObject]:
    out: list[PlexosObject] = []
    for elem in root.findall(f"{NS}t_object"):
        oid = _findtext_int(elem, "object_id")
        cid = _findtext_int(elem, "class_id")
        name = elem.findtext(f"{NS}name")
        if oid is None or cid is None or name is None:
            continue
        cat_id = _findtext_int(elem, "category_id")
        out.append(
            PlexosObject(
                object_id=oid,
                class_id=cid,
                name=name,
                category_id=cat_id,
            )
        )
    return out


def _parse_categories(root: ET.Element) -> dict[int, str]:
    """Build the ``t_category.category_id → category_name`` lookup.

    Used by :func:`plexos2gtopt.parsers.extract_generators` to classify
    generators into the canonical primary-energy taxonomy (solar / wind
    / hydro / thermal-by-fuel-family) — CEN PCP categories include
    ``"Solar Farms"``, ``"Wind Farms"``, ``"Hydro Gen Group A/B/C"``,
    and several thermal-zone variants.
    """
    out: dict[int, str] = {}
    for elem in root.findall(f"{NS}t_category"):
        cid = _findtext_int(elem, "category_id")
        name = elem.findtext(f"{NS}name") or ""
        if cid is not None:
            out[cid] = name
    return out


def _parse_collections(root: ET.Element) -> list[PlexosCollection]:
    out: list[PlexosCollection] = []
    for elem in root.findall(f"{NS}t_collection"):
        cid = _findtext_int(elem, "collection_id")
        pcid = _findtext_int(elem, "parent_class_id")
        ccid = _findtext_int(elem, "child_class_id")
        name = elem.findtext(f"{NS}name") or ""
        if cid is None or pcid is None or ccid is None:
            continue
        out.append(
            PlexosCollection(
                collection_id=cid,
                parent_class_id=pcid,
                child_class_id=ccid,
                name=name,
            )
        )
    return out


def _parse_memberships(root: ET.Element) -> list[PlexosMembership]:
    out: list[PlexosMembership] = []
    for elem in root.findall(f"{NS}t_membership"):
        mid = _findtext_int(elem, "membership_id")
        cid = _findtext_int(elem, "collection_id")
        po = _findtext_int(elem, "parent_object_id")
        co = _findtext_int(elem, "child_object_id")
        if mid is None or cid is None or po is None or co is None:
            continue
        out.append(
            PlexosMembership(
                membership_id=mid,
                collection_id=cid,
                parent_object_id=po,
                child_object_id=co,
            )
        )
    return out


def _parse_properties(root: ET.Element) -> list[PlexosProperty]:
    out: list[PlexosProperty] = []
    for elem in root.findall(f"{NS}t_property"):
        pid = _findtext_int(elem, "property_id")
        cid = _findtext_int(elem, "collection_id")
        name = elem.findtext(f"{NS}name") or ""
        if pid is None or cid is None:
            continue
        out.append(PlexosProperty(property_id=pid, collection_id=cid, name=name))
    return out


def _parse_date_map(root: ET.Element, table: str) -> dict[int, datetime]:
    """Index ``t_date_from`` / ``t_date_to`` as ``{data_id -> datetime}``."""
    out: dict[int, datetime] = {}
    for elem in root.findall(f"{NS}{table}"):
        did = _findtext_int(elem, "data_id")
        raw = elem.findtext(f"{NS}date")
        if did is None or raw is None:
            continue
        try:
            out[did] = datetime.fromisoformat(raw)
        except ValueError:
            continue
    return out


def _parse_horizon_start(root: ET.Element) -> datetime | None:
    """Decode the Horizon ``Date From`` attribute (OLE serial) to a datetime.

    PLEXOS stores the horizon start as an OLE Automation date serial
    (days since 1899-12-30) on the Horizon object's ``Date From``
    attribute.  The CEN PRGdia daily bundles carry exactly one Horizon
    with a single date, so the first match is authoritative.
    """
    horizon_cids = {
        _findtext_int(c, "class_id")
        for c in root.findall(f"{NS}t_class")
        if (c.findtext(f"{NS}name") or "") == "Horizon"
    }
    horizon_oids = {
        _findtext_int(o, "object_id")
        for o in root.findall(f"{NS}t_object")
        if _findtext_int(o, "class_id") in horizon_cids
    }
    date_from_aids = {
        _findtext_int(a, "attribute_id")
        for a in root.findall(f"{NS}t_attribute")
        if (a.findtext(f"{NS}name") or "") == "Date From"
    }
    for ad in root.findall(f"{NS}t_attribute_data"):
        if (
            _findtext_int(ad, "object_id") in horizon_oids
            and _findtext_int(ad, "attribute_id") in date_from_aids
        ):
            serial = _findtext_float(ad, "value")
            if serial is not None:
                return datetime(1899, 12, 30) + timedelta(days=serial)
    return None


#: PLEXOS class_id for the ``Timeslice`` class — the home of the recurring
#: hour-of-day / day-of-week pattern tags that scope individual ``t_data``
#: rows (separate from the dated ``t_date_from``/``t_date_to`` mechanism).
#: Verified against ``DBSEN_PRGDIARIO.xml`` (CEN PCP 2026-04-22): the
#: ``H16-20``, ``H1-8``, ``W2-6,H8-21`` tags on ``Campiche_starting`` /
#: ``Commit_*`` / ``IL_*`` UCs all carry ``class_id=76``.
_TIMESLICE_CLASS_ID = 76


def _parse_timeslice_map(root: ET.Element) -> dict[int, str]:
    """Index ``<t_text class_id=76>`` companions as ``{data_id -> tag}``.

    Each entry in the returned map is the recurring hour-of-day pattern
    (or weekday-and-hour pattern) attached to a single ``t_data`` row.
    See :class:`PlexosDataRow.timeslice` for the grammar and
    :func:`parsers._expand_timeslice` for the per-block expansion.
    """
    out: dict[int, str] = {}
    for elem in root.findall(f"{NS}t_text"):
        cid = _findtext_int(elem, "class_id")
        if cid != _TIMESLICE_CLASS_ID:
            continue
        did = _findtext_int(elem, "data_id")
        raw = elem.findtext(f"{NS}value")
        if did is None or raw is None:
            continue
        tag = raw.strip()
        if tag:
            out[did] = tag
    return out


def _parse_tags(root: ET.Element) -> dict[int, list[int]]:
    """Index ``t_tag`` as ``{data_id -> [tagged object_ids, …]}``.

    PLEXOS uses ``t_tag`` to mark a ``t_data`` row as conditional on a
    Scenario (or other tag object).  An untagged row is the unconditional
    default; a tagged row is only active when one of its tagged objects
    is enabled in the active Model.  Typical use: dual-row property
    overrides where the default value is one number and the
    scenario-conditional override is another (e.g. ``Include in ST
    Schedule = 0`` by default, ``= -1`` when ``CSFUp_ON`` is active).
    """
    out: dict[int, list[int]] = {}
    for elem in root.findall(f"{NS}t_tag"):
        did = _findtext_int(elem, "data_id")
        oid = _findtext_int(elem, "object_id")
        if did is None or oid is None:
            continue
        out.setdefault(did, []).append(oid)
    return out


def _parse_data(root: ET.Element) -> list[PlexosDataRow]:
    dfrom = _parse_date_map(root, "t_date_from")
    dto = _parse_date_map(root, "t_date_to")
    timeslice = _parse_timeslice_map(root)
    out: list[PlexosDataRow] = []
    for elem in root.findall(f"{NS}t_data"):
        did = _findtext_int(elem, "data_id")
        mid = _findtext_int(elem, "membership_id")
        pid = _findtext_int(elem, "property_id")
        val = _findtext_float(elem, "value")
        period = _findtext_int(elem, "period_id")
        if did is None or mid is None or pid is None or val is None:
            continue
        out.append(
            PlexosDataRow(
                data_id=did,
                membership_id=mid,
                property_id=pid,
                value=val,
                period_id=period,
                date_from=dfrom.get(did),
                date_to=dto.get(did),
                timeslice=timeslice.get(did),
            )
        )
    return out


# ---------------------------------------------------------------------------
# Top-level loader
# ---------------------------------------------------------------------------


@lru_cache(maxsize=4)
def _load_root_cached(xml_path: str) -> ET.Element:
    """Memoised XML parse, keyed by absolute path string."""
    logger.info("parsing PLEXOS XML %s", xml_path)
    return ET.parse(xml_path).getroot()


def load_xml(path: Path | str) -> PlexosDb:
    """Parse a PLEXOS ``MasterDataSet`` XML file into a :class:`PlexosDb`.

    Args:
        path: Filesystem path to ``DBSEN_PRGDIARIO.xml``.

    Returns:
        A populated :class:`PlexosDb` view.

    Raises:
        FileNotFoundError: If ``path`` does not exist.
        xml.etree.ElementTree.ParseError: If the XML is malformed.
    """
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"PLEXOS XML not found: {p}")
    root = _load_root_cached(str(p.resolve()))
    classes_by_id, classes_by_name = _parse_classes(root)
    return PlexosDb(
        path=p,
        classes_by_id=classes_by_id,
        classes_by_name=classes_by_name,
        objects=_parse_objects(root),
        categories_by_id=_parse_categories(root),
        collections=_parse_collections(root),
        memberships=_parse_memberships(root),
        properties=_parse_properties(root),
        # full ``t_data`` parse so extractors can pull static
        # properties (line reactance, battery SOC bounds, …) keyed by
        # (System-collection membership_id, property_id). ~72k rows on
        # the 36 MB PCP bundle, ~250 ms parse.
        data_rows=_parse_data(root),
        tag_for_data=_parse_tags(root),
        horizon_start=_parse_horizon_start(root),
    )


__all__ = [
    "PlexosCollection",
    "PlexosDataRow",
    "PlexosDb",
    "PlexosMembership",
    "PlexosObject",
    "PlexosProperty",
    "load_xml",
    "NS",
]

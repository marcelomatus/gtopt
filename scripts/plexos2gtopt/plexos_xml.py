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
    """One ``t_data`` row — numeric property value."""

    data_id: int
    membership_id: int
    property_id: int
    value: float
    period_id: int | None = None


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
    collections: list[PlexosCollection] = field(default_factory=list)
    memberships: list[PlexosMembership] = field(default_factory=list)
    properties: list[PlexosProperty] = field(default_factory=list)
    data_rows: list[PlexosDataRow] = field(default_factory=list)

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
    ) -> float:
        """Convenience: look up a single-valued System-collection property.

        Returns ``default`` when the property is undefined for this
        object (the System collection ships the class-wide default,
        which we treat as "no override"). Multi-band rows collapse to
        the lowest ``data_id`` (smallest band index by convention).
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
        return rows[0].value


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
        out.append(PlexosObject(object_id=oid, class_id=cid, name=name))
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


def _parse_data(root: ET.Element) -> list[PlexosDataRow]:
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
        collections=_parse_collections(root),
        memberships=_parse_memberships(root),
        properties=_parse_properties(root),
        # full ``t_data`` parse so extractors can pull static
        # properties (line reactance, battery SOC bounds, …) keyed by
        # (System-collection membership_id, property_id). ~72k rows on
        # the 36 MB PCP bundle, ~250 ms parse.
        data_rows=_parse_data(root),
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

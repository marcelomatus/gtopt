"""Loader for ``psrclasses.json`` — the canonical typed snapshot a PSR
SDDP study writes alongside its ``.dat`` files.

The JSON is a flat ``{collection_name: [entity, ...]}`` dictionary.
Every entity carries a ``reference_id`` (used by other entities to
cross-reference) and most carry ``code``, ``name`` and ``classType``.

This loader is intentionally schema-agnostic: it does not enumerate
known collections or attributes. Callers iterate by collection name
(e.g. ``loader.entities("PSRThermalPlant")``) and resolve cross-refs
through :meth:`PsrClassesLoader.by_reference_id`.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator


PSRCLASSES_FILENAME = "psrclasses.json"


@dataclass
class PsrClassesLoader:
    """In-memory view of a ``psrclasses.json`` file.

    Attributes:
        path: Source path of the JSON file.
        data: Raw parsed JSON, keyed by collection name (e.g.
            ``"PSRThermalPlant"``).
    """

    path: Path
    data: dict[str, list[dict[str, Any]]] = field(default_factory=dict)
    _ref_index: dict[int, dict[str, Any]] = field(default_factory=dict, repr=False)
    _ref_collection: dict[int, str] = field(default_factory=dict, repr=False)

    @classmethod
    def from_file(cls, path: str | Path) -> PsrClassesLoader:
        """Load a ``psrclasses.json`` from ``path``.

        Raises:
            FileNotFoundError: If ``path`` does not exist.
            ValueError: If the JSON top level is not a dict-of-lists.
        """
        p = Path(path)
        if not p.is_file():
            raise FileNotFoundError(f"psrclasses.json not found: {p}")
        with p.open("r", encoding="utf-8") as fh:
            raw = json.load(fh)
        if not isinstance(raw, dict):
            raise ValueError(
                f"{p}: expected top-level object, got {type(raw).__name__}"
            )
        for key, items in raw.items():
            if not isinstance(items, list):
                raise ValueError(
                    f"{p}: collection '{key}' must be a list, "
                    f"got {type(items).__name__}"
                )
        loader = cls(path=p, data=raw)
        loader._build_ref_index()
        return loader

    def _build_ref_index(self) -> None:
        """Populate :attr:`_ref_index` so cross-references resolve in O(1)."""
        for collection, items in self.data.items():
            for item in items:
                ref = item.get("reference_id")
                if isinstance(ref, int):
                    self._ref_index[ref] = item
                    self._ref_collection[ref] = collection

    @classmethod
    def from_case_dir(cls, case_dir: str | Path) -> PsrClassesLoader:
        """Convenience: locate ``psrclasses.json`` inside ``case_dir``."""
        return cls.from_file(Path(case_dir) / PSRCLASSES_FILENAME)

    def collections(self) -> list[str]:
        """Return the sorted list of collection names present."""
        return sorted(self.data.keys())

    def count(self, collection: str) -> int:
        """Return how many entities exist in ``collection`` (0 if absent)."""
        return len(self.data.get(collection, []))

    def entities(self, collection: str) -> list[dict[str, Any]]:
        """Return the list of entities in ``collection`` (empty if absent)."""
        return list(self.data.get(collection, []))

    def first(self, collection: str) -> dict[str, Any] | None:
        """Return the first entity in ``collection`` or ``None``."""
        items = self.data.get(collection)
        return items[0] if items else None

    def iter_entities(self) -> Iterator[tuple[str, dict[str, Any]]]:
        """Yield ``(collection_name, entity)`` for every entity."""
        for collection, items in self.data.items():
            for item in items:
                yield collection, item

    def by_reference_id(self, ref: int) -> dict[str, Any] | None:
        """Resolve an entity by its ``reference_id`` (None if unknown)."""
        return self._ref_index.get(ref)

    def collection_of(self, ref: int) -> str | None:
        """Return the collection name an entity ``ref`` belongs to."""
        return self._ref_collection.get(ref)


def load_psrclasses(case_dir: str | Path) -> PsrClassesLoader:
    """Module-level convenience wrapper for :meth:`from_case_dir`."""
    return PsrClassesLoader.from_case_dir(case_dir)

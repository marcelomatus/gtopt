"""``plexos2gtopt --info`` renderer.

Prints a compact summary of a PLEXOS bundle so users can sanity-check
what the converter will see without needing to crack open the 36 MB
``DBSEN_PRGDIARIO.xml`` by hand. Mirrors the ``sddp2gtopt --info``
flow.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .plexos_loader import locate_bundle
from .plexos_xml import PlexosDb, load_xml


logger = logging.getLogger(__name__)


_CORE_CLASSES = (
    "System",
    "Node",
    "Generator",
    "Fuel",
    "Battery",
    "Storage",
    "Waterway",
    "Line",
    "Region",
    "Zone",
    "Reserve",
    "Emission",
)


def _class_counts(db: PlexosDb) -> list[tuple[str, int]]:
    """Return ``[(class_name, object_count), …]`` for the core classes."""
    rows: list[tuple[str, int]] = []
    for cname in _CORE_CLASSES:
        rows.append((cname, len(db.objects_of_class(cname))))
    return rows


def _print_table(
    title: str, headers: tuple[str, ...], rows: list[tuple[str, ...]]
) -> None:
    """Print a small text-mode table; no-op when ``rows`` is empty."""
    print(title)
    if not rows:
        print("  (none)")
        return
    cols = list(zip(headers, *rows, strict=False))
    widths = [max(len(str(c)) for c in col) for col in cols]
    line = "  " + "  ".join(h.ljust(w) for h, w in zip(headers, widths, strict=False))
    print(line)
    print("  " + "  ".join("-" * w for w in widths))
    for row in rows:
        print(
            "  " + "  ".join(str(c).ljust(w) for c, w in zip(row, widths, strict=False))
        )


def display_plexos_info(options: dict[str, Any]) -> None:
    """Print a human-readable summary of the PLEXOS bundle.

    Raises:
        FileNotFoundError: If the bundle path is missing or malformed.
        ValueError: If the archive layout is not supported.
    """
    input_path = Path(options["input_bundle"])
    with locate_bundle(input_path) as bundle:
        print(f"PLEXOS bundle: {input_path}")
        print(f"Extracted to:  {bundle.root}")
        print()
        db = load_xml(bundle.xml_path)
        print(f"XML classes: {len(db.classes_by_id)}")
        print(f"XML objects: {len(db.objects)}")
        print(f"XML collections: {len(db.collections)}")
        print(f"XML memberships: {len(db.memberships)}")
        print(f"XML properties:  {len(db.properties)}")
        print()
        _print_table(
            "Core class counts:",
            ("class", "objects"),
            [(name, str(count)) for name, count in _class_counts(db)],
        )
        print()
        _print_table(
            "Top-level CSV files:",
            ("filename",),
            [(p.name,) for p in bundle.csv_paths()],
        )


__all__ = ["display_plexos_info"]

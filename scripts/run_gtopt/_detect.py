# SPDX-License-Identifier: BSD-3-Clause
"""Case type detection: PLP, gtopt, or passthrough."""

from __future__ import annotations

import enum
from pathlib import Path

# Canonical PLP files that must be present for a valid PLP case.
# plpblo.dat (block definitions) is always present in every PLP case.
_PLP_REQUIRED = ("plpblo.dat",)

# Additional PLP indicator files — presence of any confirms it's PLP.
_PLP_INDICATORS = (
    "plpbar.dat",
    "plpdem.dat",
    "plpeta.dat",
    "plpcnfce.dat",
    "plpcnfli.dat",
    "plpcosce.dat",
)

# Canonical PLEXOS (CEN PCP) object-database file inside a bundle directory.
_PLEXOS_XML = "DBSEN_PRGDIARIO.xml"

# Recognised PLEXOS archive suffixes (longest first so ``.zip.xz`` wins).
_PLEXOS_ARCHIVE_SUFFIXES = (".zip.xz", ".zip")

# Archive basename prefixes for a PLEXOS bundle (input or wrapper).
_PLEXOS_ARCHIVE_PREFIXES = ("PLEXOS", "DATOS")


class CaseType(enum.Enum):
    """Detected case type for a directory or archive argument."""

    PLP = "plp"
    GTOPT = "gtopt"
    PLEXOS = "plexos"
    PASSTHROUGH = "passthrough"


def _has_plp_file(path: Path, name: str) -> bool:
    """Check if a PLP file exists, plain or xz-compressed."""
    return (path / name).is_file() or (path / f"{name}.xz").is_file()


def _is_plexos_archive_name(name: str) -> bool:
    """True when *name* looks like a ``PLEXOS*`` / ``DATOS*`` bundle archive."""
    lower = name.lower()
    if not any(lower.endswith(sfx) for sfx in _PLEXOS_ARCHIVE_SUFFIXES):
        return False
    return any(name.startswith(p) for p in _PLEXOS_ARCHIVE_PREFIXES)


def _is_plexos_bundle(path: Path) -> bool:
    """Detect a PLEXOS bundle: an extracted dir or a recognised archive.

    - **directory**: contains ``DBSEN_PRGDIARIO.xml`` or a
      ``DATOS*.zip[.xz]`` / ``PLEXOS*.zip[.xz]`` payload.
    - **file**: a ``PLEXOS*`` / ``DATOS*`` ``.zip[.xz]`` archive.
    """
    if path.is_dir():
        if (path / _PLEXOS_XML).is_file():
            return True
        return any(_is_plexos_archive_name(p.name) for p in path.iterdir())
    if path.is_file():
        return _is_plexos_archive_name(path.name)
    return False


def detect_case_type(path: Path) -> CaseType:
    """Classify a path as a PLP, gtopt, PLEXOS case, or passthrough.

    - **PLEXOS**: a bundle directory (with ``DBSEN_PRGDIARIO.xml`` or a
      ``DATOS*``/``PLEXOS*`` archive inside) or a ``PLEXOS*``/``DATOS*``
      ``.zip[.xz]`` archive file.
    - **PLP**: directory contains ``plpblo.dat[.xz]`` plus at least one
      other ``plp*.dat[.xz]`` file.
    - **gtopt**: directory contains ``<dirname>/<dirname>.json``.
    - **passthrough**: everything else (file, non-existent, etc.).
    """
    # PLEXOS is checked first because it also recognises archive *files*
    # (PLP / gtopt detection only inspect directories).
    if _is_plexos_bundle(path):
        return CaseType.PLEXOS

    if not path.is_dir():
        return CaseType.PASSTHROUGH

    # Check for PLP case: plpblo.dat(.xz) is the canonical required file
    has_required = all(_has_plp_file(path, f) for f in _PLP_REQUIRED)
    if has_required:
        has_indicator = any(_has_plp_file(path, f) for f in _PLP_INDICATORS)
        if has_indicator:
            return CaseType.PLP

    # Check for gtopt case: dir/dir_name.json
    dir_name = path.name
    json_file = path / f"{dir_name}.json"
    if json_file.is_file():
        return CaseType.GTOPT

    return CaseType.PASSTHROUGH


def infer_gtopt_dir(plp_dir: Path) -> Path:
    """Derive gtopt output directory from a PLP directory name.

    ``plp_case_2y`` → ``gtopt_case_2y``.
    Non-``plp_`` prefixed names get a ``gtopt_`` prefix prepended.
    """
    name = plp_dir.name
    if name.startswith("plp_"):
        return plp_dir.parent / ("gtopt_" + name[4:])
    return plp_dir.parent / ("gtopt_" + name)


def plexos_stem(path: Path) -> str:
    """Bundle stem with the ``.zip[.xz]`` archive suffix stripped.

    Matches ``plexos2gtopt._resolve_output_paths``: the gtopt JSON is named
    ``<stem>.json`` and the default output directory is ``gtopt_<stem>``.
    """
    stem = path.name
    for suffix in _PLEXOS_ARCHIVE_SUFFIXES:
        if stem.lower().endswith(suffix):
            return stem[: -len(suffix)]
    return stem


def infer_plexos_gtopt_dir(bundle: Path) -> Path:
    """Derive the gtopt output directory for a PLEXOS bundle (``gtopt_<stem>``)."""
    return bundle.parent / f"gtopt_{plexos_stem(bundle)}"

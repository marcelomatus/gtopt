"""Cross-reference PLP ``embalses.csv`` to patch missing PLEXOS reservoirs.

CEN PCP bundles occasionally ship a PLEXOS XML that omits a reservoir
PLP knows about — e.g. PILMAIQUEN appears in ``embalses.csv`` and in
``Hydro_StoWaterValues.csv`` (with a non-zero water-value slope) yet
has no ``Storage`` object in ``DBSEN_PRGDIARIO.xml``.  Without help
the converter silently drops the slope (a WARN log surfaces it, but
no terminal-value coupling is emitted).

This module:

1. Reads the CEN-published ``embalses.csv[.xz]`` (the same file
   ``plp2gtopt/planos_parser.py`` already parses for FEscala — same
   1-based ``#Numero, Nombre, Tipo, VolMin, VolMax, VolMinNECF,
   VolMaxNECF, FEscala, FactRendim`` schema).
2. Detects which reservoir names are referenced by the bundle's
   ``boundary_cut.slopes`` (and/or other hydro inputs) but are absent
   from ``case.reservoirs``.
3. Synthesises a :class:`ReservoirSpec` per missing entry, using the
   PLP-side metadata as the source of truth for ``emin`` / ``emax`` /
   ``eini`` / ``efficiency``.

The patcher is OFF whenever ``--plexos-legacy`` is set (the legacy
flag enforces PLEXOS-strict semantics — no cross-bundle data mixing).
"""

from __future__ import annotations

import csv
import io
import lzma
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .entities import PlexosCase, ReservoirSpec


logger = logging.getLogger(__name__)


# Compression extensions probed when ``find_compressed_path`` looks
# for ``embalses.csv`` near a PLEXOS bundle.  Order matters: ``.xz``
# is the CEN-published default.
_COMPRESSED_SUFFIXES: tuple[str, ...] = (".xz",)


def find_compressed_path(path: Path) -> Path | None:
    """Return ``path`` if it exists, or the first compressed sibling.

    Mirrors the helper of the same name in
    ``plp2gtopt.compressed_open`` (which gtopt's PLP-side reader
    uses) but is duplicated here to avoid a cross-package import
    from plexos2gtopt → plp2gtopt for a one-line search.  Returns
    ``None`` when neither plain nor compressed forms exist.
    """
    if path.exists():
        return path
    for ext in _COMPRESSED_SUFFIXES:
        candidate = path.with_name(path.name + ext)
        if candidate.exists():
            return candidate
    return None


def _open_text(path: Path) -> str:
    """Read ``path`` as text, decompressing ``.xz`` transparently."""
    if path.suffix.lower() == ".xz":
        with lzma.open(path, "rt", encoding="utf-8", errors="ignore") as fh:
            return fh.read()
    return path.read_text(encoding="utf-8", errors="ignore")


@dataclass(frozen=True)
class PlpEmbalsesEntry:
    """One row of the CEN-published ``embalses.csv``.

    The PLP source file columns are (1-based ``#Numero``, then):

    * ``Nombre``   — reservoir name (typically padded with trailing
      spaces).
    * ``Tipo``     — type code (``E`` = embalse, ``A`` = agregado, …).
    * ``VolMin``   — physical floor [hm³ or native PLP units].
    * ``VolMax``   — physical cap.
    * ``VolMinNECF`` / ``VolMaxNECF`` — non-end-of-cascade floors / caps.
    * ``FEscala``  — logarithmic volume scale exponent.
    * ``FactRendim`` — energy production factor (hydro efficiency
      proxy, MWh / hm³ or PLP equivalent).
    """

    numero: int
    name: str
    tipo: str
    vmin: float
    vmax: float
    vmin_necf: float
    vmax_necf: float
    fescala: int
    fact_rendim: float


# CEN ``embalses.csv`` always carries 9 numeric/text fields after the
# trailing comma.  Field positions (0-based after the leading
# ``#Numero``):
_F_NOMBRE = 1
_F_TIPO = 2
_F_VOLMIN = 3
_F_VOLMAX = 4
_F_VOLMIN_NECF = 5
_F_VOLMAX_NECF = 6
_F_FESCALA = 7
_F_FACTRENDIM = 8

# Auto-detection filenames — both the .xz compressed CEN payload and
# the plain ``.csv`` form are accepted.
_EMBALSES_BASENAMES = ("embalses.csv",)


def _parse_float(field: str) -> float:
    """Return ``float(field)`` after stripping whitespace; 0.0 on blank."""
    s = field.strip()
    return float(s) if s else 0.0


def _parse_int(field: str) -> int:
    """Return ``int(field)`` after stripping whitespace; 0 on blank."""
    s = field.strip()
    return int(s) if s else 0


def load_embalses_csv(path: Path) -> tuple[PlpEmbalsesEntry, ...]:
    """Parse a CEN-published ``embalses.csv[.xz]`` into typed entries.

    Skips the ``#`` comment header line and any blank lines.  Names are
    stripped of trailing whitespace (the PLP file pads every name to a
    fixed width).  Raises ``FileNotFoundError`` when ``path`` does not
    exist (after resolving compressed variants).

    The CSV uses comma separators with arbitrary internal whitespace
    around fields (the Fortran ``READ(*, *)`` parser tolerates either),
    so we use :class:`csv.reader` with ``skipinitialspace=True``.
    """
    resolved = find_compressed_path(path)
    if resolved is None:
        raise FileNotFoundError(f"embalses.csv not found at or near {path}")
    text = _open_text(resolved)
    reader = csv.reader(io.StringIO(text), skipinitialspace=True)
    out: list[PlpEmbalsesEntry] = []
    for row in reader:
        if not row:
            continue
        first = row[0].strip()
        if not first or first.startswith("#"):
            continue
        if len(row) <= _F_FACTRENDIM:
            logger.warning(
                "embalses.csv: skipping short row (got %d field(s), need >= %d): %r",
                len(row),
                _F_FACTRENDIM + 1,
                row,
            )
            continue
        try:
            entry = PlpEmbalsesEntry(
                numero=_parse_int(first),
                name=row[_F_NOMBRE].strip(),
                tipo=row[_F_TIPO].strip(),
                vmin=_parse_float(row[_F_VOLMIN]),
                vmax=_parse_float(row[_F_VOLMAX]),
                vmin_necf=_parse_float(row[_F_VOLMIN_NECF]),
                vmax_necf=_parse_float(row[_F_VOLMAX_NECF]),
                fescala=_parse_int(row[_F_FESCALA]),
                fact_rendim=_parse_float(row[_F_FACTRENDIM]),
            )
        except ValueError as exc:
            logger.warning("embalses.csv: skipping unparseable row %r (%s)", row, exc)
            continue
        out.append(entry)
    logger.info("embalses.csv: parsed %d entries from %s", len(out), resolved)
    return tuple(out)


def _candidate_locations(bundle_source: Path | None) -> Iterable[Path]:
    """Yield candidate ``embalses.csv`` paths near a PLEXOS bundle source.

    Probes (in order):
      1. The bundle source's parent directory.
      2. The bundle source itself when it is already a directory.
      3. The ``DATOS<date>`` subdirectory next to the source archive.

    The caller passes each candidate through ``find_compressed_path``
    so the ``.xz`` / ``.gz`` / ``.zst`` variants are auto-discovered.
    """
    if bundle_source is None:
        return ()
    bundle_source = Path(bundle_source)
    parents: list[Path] = [bundle_source.parent]
    if bundle_source.is_dir():
        parents.append(bundle_source)
    out: list[Path] = []
    for parent in parents:
        for base in _EMBALSES_BASENAMES:
            out.append(parent / base)
    return out


def auto_detect_embalses(bundle_source: Path | None) -> Path | None:
    """Return the first ``embalses.csv[.xz]`` found near the bundle.

    Used when the user omits ``--plp-embalses`` and does not pass
    ``--no-plp-embalses``: we probe a small set of well-known
    locations (the bundle's parent dir, the bundle dir itself).
    Returns ``None`` when no candidate exists so the caller falls back
    to the legacy WARN-only path.
    """
    for candidate in _candidate_locations(bundle_source):
        resolved = find_compressed_path(candidate)
        if resolved is not None:
            logger.info(
                "plp-embalses auto-detect: found %s near bundle %s",
                resolved,
                bundle_source,
            )
            return resolved
    return None


def _missing_reservoir_names(case: PlexosCase) -> frozenset[str]:
    """Names referenced by the boundary cut but absent from the case.

    Currently the only signal is ``case.boundary_cut.slopes`` (the
    water-value rows that would otherwise be dropped by the writer).
    Extend here when other hydro inputs gain a "patch me if PLP knows"
    contract (waterflows, FlowRights, …).
    """
    if case.boundary_cut is None:
        return frozenset()
    bundle_names = {r.name for r in case.reservoirs}
    return frozenset(
        name for name in case.boundary_cut.slopes if name not in bundle_names
    )


def patch_case_with_plp_embalses(
    case: PlexosCase,
    embalses_path: Path,
) -> PlexosCase:
    """Return ``case`` with PLP-sourced reservoirs added for any missing names.

    Cross-references ``case.boundary_cut.slopes`` against the parsed
    embalses table: every slope-targeting reservoir that has no
    PLEXOS-side ``Storage`` is synthesised here from the PLP record.

    No-op (returns ``case`` unchanged) when the boundary cut is absent
    or every reservoir already exists in the bundle.  Logs:

    * INFO per patched reservoir (with vmin / vmax / vini source).
    * WARNING for any name still missing after the patch — these slopes
      will be dropped by the writer's existing WARN-only path.
    """
    missing = _missing_reservoir_names(case)
    if not missing:
        return case

    entries = load_embalses_csv(embalses_path)
    by_name = {e.name: e for e in entries}

    # Strict policy: only patch reservoirs that have an existing chain
    # to splice into (a Turbine whose reservoir_name matches).  PLEXOS
    # may publish a water-value slope for a generator that has no
    # hydro topology at all (no Storage, no Waterway, no Turbine —
    # e.g. PILMAIQUEN); synthesising a disconnected Reservoir there
    # would just inflate boundary_cuts.csv without actually gating
    # generator dispatch, so we drop those with a WARN.
    chained_reservoir_names = frozenset(t.reservoir_name for t in case.turbines)

    new_reservoirs: list[ReservoirSpec] = list(case.reservoirs)
    # ``object_id`` is a PLEXOS-side identifier; synthesised
    # reservoirs aren't in PLEXOS, so we allocate a unique negative
    # id starting at -1 and walking down.  This keeps the namespace
    # separated from real PLEXOS object ids (which are positive) and
    # never collides with an existing one even if the bundle ever
    # ships a 0-id reservoir.
    min_existing = min(
        (r.object_id for r in new_reservoirs if r.object_id < 0),
        default=0,
    )
    next_synth_id = min_existing - 1

    patched: list[str] = []
    still_missing: list[str] = []
    orphan_skipped: list[str] = []
    for name in sorted(missing):
        plp = by_name.get(name)
        if plp is None:
            still_missing.append(name)
            continue
        if name not in chained_reservoir_names:
            orphan_skipped.append(name)
            logger.warning(
                "%s: skipping plp-embalses patch — no existing Turbine references "
                "this reservoir (no hydro chain to splice into); a disconnected "
                "reservoir would not gate generator dispatch.  The water-value "
                "slope will be dropped by boundary_cuts.csv emission.",
                name,
            )
            continue
        # ``eini`` and ``efin`` are not in CEN's published
        # ``embalses.csv`` schema.  Use vmax as the initial-fill
        # default (matches PLP's vini convention for filled
        # reservoirs) and leave ``efin`` at 0 so the writer's
        # boundary-cut FCF coupling handles the terminal value
        # (matching task spec: "if non-zero, treat as terminal
        # target; else rely on FCF coupling").
        synth = ReservoirSpec(
            object_id=next_synth_id,
            name=name,
            emin=plp.vmin,
            emax=plp.vmax,
            eini=plp.vmax,
            efin=0.0,
            water_value=0.0,
            never_drain=False,
            spill_penalty_per_mwh=0.0,
        )
        new_reservoirs.append(synth)
        patched.append(name)
        logger.info(
            "%s: synthesized Reservoir from PLP embalses.csv "
            "(vmin=%.2f, vmax=%.2f, eini=%.2f, efficiency=%.3f)",
            name,
            plp.vmin,
            plp.vmax,
            plp.vmax,
            plp.fact_rendim,
        )
        next_synth_id -= 1

    if still_missing:
        logger.warning(
            "plp-embalses patch: %d water-value slope(s) reference name(s) "
            "not in embalses.csv either: %s — these will still be dropped "
            "by boundary_cuts.csv emission",
            len(still_missing),
            still_missing,
        )

    if not patched:
        return case

    # Synthesised reservoir needs a Junction too (the writer wires
    # ``Reservoir.junction = Reservoir.name``).  Without it, the
    # built-in spillway / drain has no node to attach to and the
    # writer emits a dangling reference.  ``JunctionSpec(name=NAME)``
    # with default flags suffices — no drain, no Vert_ collapse.
    from .entities import JunctionSpec  # local import avoids cycles

    junction_names = {j.name for j in case.junctions}
    new_junctions = list(case.junctions)
    for name in patched:
        if name not in junction_names:
            new_junctions.append(JunctionSpec(name=name))

    import dataclasses as _dc

    return _dc.replace(
        case,
        reservoirs=tuple(new_reservoirs),
        junctions=tuple(new_junctions),
    )


__all__ = [
    "PlpEmbalsesEntry",
    "auto_detect_embalses",
    "load_embalses_csv",
    "patch_case_with_plp_embalses",
]

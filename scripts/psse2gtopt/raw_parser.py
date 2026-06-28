"""PSS/E RAW power-flow file reader.

Parses the classic comma-separated PSS/E ``.raw`` format (revisions 32
and 33) into the typed dataclasses of :mod:`psse2gtopt.entities`.

The RAW layout is a fixed sequence of sections, each terminated by a
line whose first comma-field is the single character ``0`` (usually
followed by ``/ END OF … DATA, BEGIN … DATA``).  The section order is
part of the format, so we read them positionally rather than trusting
the (optional, sometimes mojibake) end-of-section comments::

    case-id line, title 1, title 2
    BUS … 0 /
    LOAD … 0 /
    FIXED SHUNT … 0 /
    GENERATOR … 0 /
    BRANCH … 0 /
    TRANSFORMER … 0 /      (multi-line records: 4 lines / 2-winding,
                            5 lines / 3-winding)
    AREA … (and the remaining sections, which we don't need)

Reactances are returned in **per-unit on the system MVA base**: PSS/E
branch reactances already are, and transformer reactances are converted
up front according to the per-record ``CZ`` impedance code so callers
never have to deal with PSS/E's I/O codes.

Files are read as Latin-1 — PSS/E writes Windows-1252 / Latin-1 text
(accented Spanish in the Guatemalan titles) which is a strict superset
of ASCII for the numeric payload we care about.
"""

from __future__ import annotations

import logging
from pathlib import Path

from .entities import (
    BranchSpec,
    BusSpec,
    CaseSpec,
    GenSpec,
    LoadSpec,
    PsseCase,
    TransformerSpec,
)


logger = logging.getLogger(__name__)


# Minimum |reactance| (p.u.) emitted for a branch.  PSS/E bus-tie
# breakers and very short jumpers can have X == 0, which would make the
# DC flow equation f = (θ_a − θ_b) / X singular.  We floor the
# magnitude (preserving sign for the rare series-capacitor) to a small
# but finite value.
_MIN_REACTANCE = 1.0e-5

# PSS/E sentinel meaning "no rating / unlimited".  Mapped to a large
# finite gtopt ``tmax`` so the DC OPF doesn't artificially cap the flow.
_UNLIMITED_RATING = 99999.0


def _to_float(token: str) -> float:
    """Parse a PSS/E numeric token, tolerating blanks and stray quotes."""
    token = token.strip().strip("'").strip()
    if not token:
        return 0.0
    try:
        return float(token)
    except ValueError:
        return 0.0


def _to_int(token: str) -> int:
    """Parse a PSS/E integer token (via float to absorb ``1.0`` forms)."""
    return int(_to_float(token))


def _unquote(token: str) -> str:
    """Strip surrounding quotes and whitespace from a name field."""
    return token.strip().strip("'").strip()


def _split(line: str) -> list[str]:
    """Split a RAW record on commas (the PSS/E field separator)."""
    return line.split(",")


def _is_terminator(line: str) -> bool:
    """True for a ``0 /`` section-terminator line.

    A data record always starts with a non-zero bus number or a
    floating-point continuation token (e.g. ``0.00000E+0``), so the
    terminator is unambiguously the line whose first token is exactly
    ``"0"``.  The terminator usually reads ``0 / END OF … DATA`` with no
    comma before the ``/``, so we strip any trailing ``/comment`` from
    the first comma-field before comparing.
    """
    if not line.strip():
        return False
    first = _split(line)[0].strip()
    token = first.split("/", 1)[0].strip()
    return token == "0"


def _read_line_section(lines: list[str], pos: int) -> tuple[list[str], int]:
    """Collect one-record-per-line section starting at ``pos``.

    Returns ``(records, new_pos)`` where ``new_pos`` points just past
    the terminator line.
    """
    records: list[str] = []
    n = len(lines)
    while pos < n:
        line = lines[pos]
        if _is_terminator(line):
            return records, pos + 1
        if line.strip():
            records.append(line)
        pos += 1
    return records, pos


def _parse_case_id(line: str) -> CaseSpec:
    """Parse the first RAW line: ``IC, SBASE, REV, XFRRAT, NXFRAT, BASFRQ``.

    The trailing ``/ comment`` (PSS/E version + timestamp) is ignored.
    """
    payload = line.split("/", 1)[0]
    fields = _split(payload)
    sbase = _to_float(fields[1]) if len(fields) > 1 else 100.0
    rev = _to_int(fields[2]) if len(fields) > 2 else 33
    base_freq = _to_float(fields[5]) if len(fields) > 5 else 60.0
    return CaseSpec(
        sbase=sbase if sbase > 0 else 100.0,
        rev=rev,
        base_freq=base_freq if base_freq > 0 else 60.0,
    )


def _parse_buses(records: list[str]) -> list[BusSpec]:
    """BUS: ``I, NAME, BASKV, IDE, AREA, ZONE, OWNER, VM, VA, …``."""
    out: list[BusSpec] = []
    for rec in records:
        f = _split(rec)
        if len(f) < 4:
            continue
        out.append(
            BusSpec(
                number=_to_int(f[0]),
                name=_unquote(f[1]),
                base_kv=_to_float(f[2]),
                ide=_to_int(f[3]),
                area=_to_int(f[4]) if len(f) > 4 else 1,
                zone=_to_int(f[5]) if len(f) > 5 else 1,
            )
        )
    return out


def _parse_loads(records: list[str]) -> list[LoadSpec]:
    """LOAD: ``I, ID, STATUS, AREA, ZONE, PL, QL, …``."""
    out: list[LoadSpec] = []
    for rec in records:
        f = _split(rec)
        if len(f) < 7:
            continue
        out.append(
            LoadSpec(
                bus=_to_int(f[0]),
                ident=_unquote(f[1]),
                status=_to_int(f[2]),
                pl=_to_float(f[5]),
                ql=_to_float(f[6]),
            )
        )
    return out


def _parse_gens(records: list[str]) -> list[GenSpec]:
    """GENERATOR: ``I, ID, PG, QG, QT, QB, VS, IREG, MBASE, …, STAT, RMPCT, PT, PB, …``."""
    out: list[GenSpec] = []
    for rec in records:
        f = _split(rec)
        if len(f) < 18:
            continue
        out.append(
            GenSpec(
                bus=_to_int(f[0]),
                ident=_unquote(f[1]),
                pg=_to_float(f[2]),
                mbase=_to_float(f[8]),
                status=_to_int(f[14]),
                pmax=_to_float(f[16]),
                pmin=_to_float(f[17]),
            )
        )
    return out


def _parse_branches(records: list[str]) -> list[BranchSpec]:
    """BRANCH: ``I, J, CKT, R, X, B, RATEA, RATEB, RATEC, …, ST, …``."""
    out: list[BranchSpec] = []
    for rec in records:
        f = _split(rec)
        if len(f) < 14:
            continue
        out.append(
            BranchSpec(
                from_bus=_to_int(f[0]),
                to_bus=_to_int(f[1]),
                ckt=_unquote(f[2]),
                r=_to_float(f[3]),
                x=_to_float(f[4]),
                b=_to_float(f[5]),
                rate_a=_to_float(f[6]),
                rate_b=_to_float(f[7]) if len(f) > 7 else 0.0,
                rate_c=_to_float(f[8]) if len(f) > 8 else 0.0,
                status=_to_int(f[13]),
            )
        )
    return out


def _winding_x_to_system_base(
    x: float, cz: int, sbase_wind: float, sbase_sys: float
) -> float:
    """Convert a transformer winding reactance to system-base per-unit.

    PSS/E ``CZ`` impedance I/O code:

    * ``1`` — already in p.u. on the system base → returned unchanged.
    * ``2`` — in p.u. on the winding MVA base → scaled by
      ``sbase_sys / sbase_wind``.
    * ``3`` — load-loss (W) + |Z| (p.u.) form; not seen in the
      Guatemalan cases.  We treat the value as system-base p.u. and log
      a warning so it surfaces if a future case uses it.
    """
    if cz == 2:
        base = sbase_wind if sbase_wind > 0 else sbase_sys
        return x * (sbase_sys / base)
    if cz == 3:
        logger.warning(
            "transformer CZ=3 (load-loss form) not fully supported; using raw X"
        )
    return x


def _winding_ratings(winding_fields: list[str]) -> tuple[float, float, float]:
    """Extract (RATA, RATB, RATC) MVA ratings from a transformer winding line."""
    ra = _to_float(winding_fields[3]) if len(winding_fields) > 3 else 0.0
    rb = _to_float(winding_fields[4]) if len(winding_fields) > 4 else 0.0
    rc = _to_float(winding_fields[5]) if len(winding_fields) > 5 else 0.0
    return ra, rb, rc


def _read_transformer_section(
    lines: list[str], pos: int, sbase_sys: float
) -> tuple[list[TransformerSpec], int]:
    """Parse the multi-line TRANSFORMER section.

    Each record is 4 lines (2-winding, ``K == 0``) or 5 lines
    (3-winding).  Line 1 holds the bus triple + ``CZ`` + status; line 2
    holds the winding-pair impedances; the remaining lines hold the
    per-winding tap / rating data (we keep ``RATA``).
    """
    out: list[TransformerSpec] = []
    n = len(lines)
    while pos < n:
        line1 = lines[pos]
        if _is_terminator(line1):
            return out, pos + 1
        if not line1.strip():
            pos += 1
            continue
        f1 = _split(line1)
        bus_i = _to_int(f1[0])
        bus_j = _to_int(f1[1])
        bus_k = _to_int(f1[2])
        ckt = _unquote(f1[3]) if len(f1) > 3 else "1"
        cz = _to_int(f1[5]) if len(f1) > 5 else 1
        name = _unquote(f1[10]) if len(f1) > 10 else ""
        status = _to_int(f1[11]) if len(f1) > 11 else 1
        two_winding = bus_k == 0

        f2 = _split(lines[pos + 1]) if pos + 1 < n else []

        if two_winding:
            x12 = _winding_x_to_system_base(
                _to_float(f2[1]) if len(f2) > 1 else 0.0,
                cz,
                _to_float(f2[2]) if len(f2) > 2 else 0.0,
                sbase_sys,
            )
            w1 = _split(lines[pos + 2]) if pos + 2 < n else []
            ra1, rb1, rc1 = _winding_ratings(w1)
            out.append(
                TransformerSpec(
                    bus_i=bus_i,
                    bus_j=bus_j,
                    bus_k=0,
                    ckt=ckt,
                    name=name,
                    status=status,
                    windings=2,
                    x12=x12,
                    rate1=ra1,
                    rate1_b=rb1,
                    rate1_c=rc1,
                )
            )
            pos += 4
        else:
            sb12 = _to_float(f2[2]) if len(f2) > 2 else 0.0
            sb23 = _to_float(f2[5]) if len(f2) > 5 else 0.0
            sb31 = _to_float(f2[8]) if len(f2) > 8 else 0.0
            x12 = _winding_x_to_system_base(
                _to_float(f2[1]) if len(f2) > 1 else 0.0, cz, sb12, sbase_sys
            )
            x23 = _winding_x_to_system_base(
                _to_float(f2[4]) if len(f2) > 4 else 0.0, cz, sb23, sbase_sys
            )
            x31 = _winding_x_to_system_base(
                _to_float(f2[7]) if len(f2) > 7 else 0.0, cz, sb31, sbase_sys
            )
            w1 = _split(lines[pos + 2]) if pos + 2 < n else []
            w2 = _split(lines[pos + 3]) if pos + 3 < n else []
            w3 = _split(lines[pos + 4]) if pos + 4 < n else []
            ra1, rb1, rc1 = _winding_ratings(w1)
            ra2, rb2, rc2 = _winding_ratings(w2)
            ra3, rb3, rc3 = _winding_ratings(w3)
            out.append(
                TransformerSpec(
                    bus_i=bus_i,
                    bus_j=bus_j,
                    bus_k=bus_k,
                    ckt=ckt,
                    name=name,
                    status=status,
                    windings=3,
                    x12=x12,
                    x23=x23,
                    x31=x31,
                    rate1=ra1,
                    rate2=ra2,
                    rate3=ra3,
                    rate1_b=rb1,
                    rate2_b=rb2,
                    rate3_b=rb3,
                    rate1_c=rc1,
                    rate2_c=rc2,
                    rate3_c=rc3,
                )
            )
            pos += 5
    return out, pos


def floor_reactance(x: float) -> float:
    """Clamp |x| to a finite minimum, preserving sign (see ``_MIN_REACTANCE``)."""
    if abs(x) < _MIN_REACTANCE:
        return _MIN_REACTANCE if x >= 0 else -_MIN_REACTANCE
    return x


def rating_to_tmax(rate_a: float) -> float:
    """Map a PSS/E MVA rating to a gtopt ``tmax``; 0 → large finite cap."""
    return rate_a if rate_a > 0 else _UNLIMITED_RATING


def parse_raw(path: str | Path) -> PsseCase:
    """Parse a PSS/E RAW file into a :class:`PsseCase`.

    Args:
        path: Path to the ``.raw`` file.

    Returns:
        The fully parsed case (bus / load / generator / branch /
        transformer sections; later sections are skipped).

    Raises:
        FileNotFoundError: If ``path`` does not exist.
        ValueError: If the file is too short to hold a case-id line, or
            if it is a PSS/E v34+/RAWX-style file (a different layout the
            classic reader cannot parse — see below).
    """
    from gtopt_shared.compressed_open import (  # pylint: disable=import-outside-toplevel
        find_compressed_path,
        read_text,
    )

    path = Path(path)
    if find_compressed_path(path) is None:
        raise FileNotFoundError(f"PSS/E RAW file not found: {path}")

    # Transparent .xz/.gz/… decompression (and plain → compressed fallback).
    lines = read_text(path, encoding="latin-1", errors="replace").splitlines()
    if len(lines) < 3:
        raise ValueError(f"{path}: not a PSS/E RAW file (fewer than 3 lines)")

    # PSS/E v35 writes a leading ``@!IC,SBASE,...`` column-header comment
    # before the case-id line; the real case-id is then the next line.
    # The v34+ layout also inserts a system-wide data block (GENERAL /
    # GAUSS / NEWTON / RATING ...) between the titles and the bus data and
    # reshapes the generator/branch records — none of which the classic
    # rev-32/33 reader understands.  Detect and reject up front rather
    # than silently emitting a garbage network.
    case_id_idx = 1 if lines[0].lstrip().startswith("@!") else 0
    case = _parse_case_id(lines[case_id_idx])
    if case_id_idx == 1 or case.rev >= 34:
        raise ValueError(
            f"{path}: PSS/E RAW revision {case.rev} (v34+/RAWX layout) is not "
            "supported by the classic-format reader. Export the case as PSS/E "
            "v33 (or v32) RAW — e.g. use the .raw from a PSS(R)Ev33/PSS(R)Ev32 "
            "directory instead of PSS(R)Ev35."
        )
    case.title1 = lines[case_id_idx + 1].strip()
    case.title2 = lines[case_id_idx + 2].strip()
    if case.rev < 32:
        logger.warning(
            "RAW revision %d is older than the tested range (32/33); "
            "parsing with the classic-format reader anyway",
            case.rev,
        )

    pos = case_id_idx + 3
    bus_recs, pos = _read_line_section(lines, pos)
    load_recs, pos = _read_line_section(lines, pos)
    _shunt_recs, pos = _read_line_section(lines, pos)  # fixed shunt — unused
    gen_recs, pos = _read_line_section(lines, pos)
    branch_recs, pos = _read_line_section(lines, pos)
    transformers, pos = _read_transformer_section(lines, pos, case.sbase)

    psse = PsseCase(
        case=case,
        buses=_parse_buses(bus_recs),
        loads=_parse_loads(load_recs),
        gens=_parse_gens(gen_recs),
        branches=_parse_branches(branch_recs),
        transformers=transformers,
    )
    logger.info(
        "parsed %s (REV %d): %d buses, %d loads, %d gens, %d branches, %d transformers",
        path.name,
        case.rev,
        len(psse.buses),
        len(psse.loads),
        len(psse.gens),
        len(psse.branches),
        len(psse.transformers),
    )
    return psse

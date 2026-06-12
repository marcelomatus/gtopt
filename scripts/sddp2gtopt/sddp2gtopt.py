"""High-level conversion entry-points for ``sddp2gtopt``.

The functions here are the public façade used by :mod:`sddp2gtopt.main`:

* :func:`validate_sddp_case` — read ``psrclasses.json`` and perform
  light schema sanity checks.
* :func:`convert_sddp_case` — full conversion to gtopt JSON.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .gtopt_writer import build_planning, write_planning
from .parsers import (
    parse_demands,
    parse_hydro_plants,
    parse_study,
    parse_systems,
    parse_thermal_plants,
)
from .psrclasses_loader import load_psrclasses


logger = logging.getLogger(__name__)


REQUIRED_COLLECTIONS = ("PSRStudy", "PSRSystem")


def validate_sddp_case(options: dict[str, Any]) -> bool:
    """Validate an SDDP case directory.

    Checks that ``psrclasses.json`` exists and parses, and that the
    minimal required collections are present and non-empty.

    Args:
        options: Conversion options. Recognised keys:

            * ``input_dir`` (required) — path to the SDDP case dir.

    Returns:
        ``True`` if the case looks well-formed, ``False`` otherwise.
        Failures are logged at ``ERROR`` level — callers should set up
        logging before invoking this function.
    """
    input_dir = options.get("input_dir")
    if input_dir is None:
        logger.error("validate_sddp_case: 'input_dir' option is required")
        return False
    try:
        loader = load_psrclasses(input_dir)
    except (FileNotFoundError, ValueError) as exc:
        logger.error("validate failed: %s", exc)
        return False

    ok = True
    for required in REQUIRED_COLLECTIONS:
        if loader.count(required) == 0:
            logger.error(
                "validate failed: collection '%s' is missing or empty",
                required,
            )
            ok = False

    if ok:
        logger.info("validation OK: %s", loader.path)
    return ok


def _resolve_output_paths(
    input_dir: Path,
    output_dir: Path | None,
    output_file: Path | None,
    name: str | None,
) -> tuple[Path, Path, str]:
    """Compute ``(output_dir, output_file, planning_name)`` for the run.

    Mirrors plp2gtopt's inference: if the input dir starts with
    ``sddp_``, replace the prefix with ``gtopt_`` for the default
    output dir. Otherwise default to ``./output``.
    """
    if output_dir is None:
        if input_dir.name.startswith("sddp_"):
            output_dir = input_dir.parent / ("gtopt_" + input_dir.name[5:])
        else:
            output_dir = input_dir.parent / f"gtopt_{input_dir.name}"
    if output_file is None:
        output_file = output_dir / f"{output_dir.name}.json"
    planning_name = name or output_dir.name
    return output_dir, output_file, planning_name


def convert_sddp_case(options: dict[str, Any]) -> int:
    """Convert an SDDP case directory to gtopt JSON.

    Reads ``psrclasses.json``, parses each known collection, and
    writes a single-bus gtopt planning JSON to disk.

    Args:
        options: Conversion options. Recognised keys:

            * ``input_dir`` (required) — path to the SDDP case dir.
            * ``output_dir`` — where to put the planning (inferred
              from ``input_dir`` when missing).
            * ``output_file`` — explicit JSON path; default is
              ``<output_dir>/<output_dir.name>.json``.
            * ``name`` — planning name (default: output dir name).
            * ``use_single_bus`` — overrides the default ``True``.

    Returns:
        Number of CRITICAL findings raised by post-conversion
        validation. ``0`` means success.
    """
    raw_input = options.get("input_dir")
    if raw_input is None:
        raise ValueError("convert_sddp_case: 'input_dir' option is required")
    input_dir = Path(raw_input)
    loader = load_psrclasses(input_dir)

    study = parse_study(loader)
    systems = parse_systems(loader)
    if not systems:
        raise ValueError(f"{loader.path}: no PSRSystem entities — cannot convert")
    thermals = parse_thermal_plants(loader)
    hydros = parse_hydro_plants(loader)
    demands = parse_demands(loader)

    output_dir, output_file, planning_name = _resolve_output_paths(
        input_dir,
        options.get("output_dir"),
        options.get("output_file"),
        options.get("name"),
    )

    planning = build_planning(
        study=study,
        systems=systems,
        thermals=thermals,
        hydros=hydros,
        demands=demands,
        name=planning_name,
    )

    write_planning(planning, output_file)
    logger.info(
        "converted %s → %s "
        "(stages=%d, blocks/stage=%d, thermals=%d, hydros=%d, demands=%d)",
        input_dir,
        output_file,
        study.num_stages,
        study.num_blocks,
        len(thermals),
        len(hydros),
        len(demands),
    )
    # Auto-create the output dir even if no extra files are written so
    # downstream tools that mkdir-on-write don't surprise the user.
    output_dir.mkdir(parents=True, exist_ok=True)
    return 0

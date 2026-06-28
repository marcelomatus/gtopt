"""High-level conversion entry-points for ``psse2gtopt``.

The functions here are the public façade used by :mod:`psse2gtopt.main`:

* :func:`resolve_raw_file` — pick the ``.raw`` to convert from a file or
  a directory of snapshots.
* :func:`validate_psse_case` — parse a RAW and run light sanity checks.
* :func:`convert_psse_case` — full conversion to a gtopt JSON planning.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .gtopt_writer import build_planning, write_planning
from .raw_parser import parse_raw


logger = logging.getLogger(__name__)


def _find_raw_files(directory: Path) -> list[Path]:
    """Return the ``.raw`` files in ``directory``, sorted by name."""
    return sorted(
        (p for p in directory.iterdir() if p.is_file() and p.suffix.lower() == ".raw"),
        key=lambda p: p.name.lower(),
    )


def resolve_raw_file(input_path: Path, selector: str | None = None) -> Path:
    """Resolve the concrete ``.raw`` file to convert.

    Args:
        input_path: A ``.raw`` file, or a directory containing one or
            more ``.raw`` snapshots.
        selector: When ``input_path`` is a directory, a substring used
            to pick a specific ``.raw`` (case-insensitive).  Ignored for
            a direct file path.

    Returns:
        The resolved ``.raw`` path.

    Raises:
        FileNotFoundError: If the path does not exist or a directory
            holds no ``.raw`` files.
        ValueError: If ``selector`` matches zero or more than one file.
    """
    if not input_path.exists():
        raise FileNotFoundError(f"input path does not exist: {input_path}")

    if input_path.is_file():
        return input_path

    raws = _find_raw_files(input_path)
    if not raws:
        raise FileNotFoundError(f"no .raw files found in directory: {input_path}")

    if selector is not None:
        matches = [p for p in raws if selector.lower() in p.name.lower()]
        if not matches:
            available = ", ".join(p.name for p in raws)
            raise ValueError(
                f"--raw '{selector}' matched no file in {input_path} "
                f"(available: {available})"
            )
        if len(matches) > 1:
            ambiguous = ", ".join(p.name for p in matches)
            raise ValueError(f"--raw '{selector}' is ambiguous: {ambiguous}")
        return matches[0]

    chosen = raws[0]
    if len(raws) > 1:
        logger.warning(
            "%d .raw files in %s; converting '%s' (use --raw to pick another: %s)",
            len(raws),
            input_path,
            chosen.name,
            ", ".join(p.name for p in raws),
        )
    return chosen


def validate_psse_case(options: dict[str, Any]) -> bool:
    """Validate a PSS/E RAW case.

    Checks that the resolved ``.raw`` parses and contains at least one
    non-isolated bus.  Failures are logged at ``ERROR`` level.

    Returns:
        ``True`` if the case looks well-formed, ``False`` otherwise.
    """
    raw_input = options.get("input")
    if raw_input is None:
        logger.error("validate_psse_case: 'input' option is required")
        return False
    try:
        raw_file = resolve_raw_file(Path(raw_input), options.get("raw"))
        case = parse_raw(raw_file)
    except (FileNotFoundError, ValueError) as exc:
        logger.error("validate failed: %s", exc)
        return False

    live = [b for b in case.buses if not b.is_isolated]
    if not live:
        logger.error("validate failed: %s has no in-service buses", raw_file)
        return False

    logger.info("validation OK: %s (%d in-service buses)", raw_file, len(live))
    return True


def _default_output_dir(raw_file: Path, input_path: Path) -> Path:
    """Infer the output directory from the input.

    ``gtopt_<raw-stem>`` next to the input — mirrors plp2gtopt's
    ``gtopt_<case>`` inference.
    """
    parent = input_path.parent if input_path.is_file() else input_path
    return parent / f"gtopt_{raw_file.stem}"


def convert_psse_case(options: dict[str, Any]) -> int:
    """Convert a PSS/E RAW case to a gtopt JSON planning.

    Recognised ``options`` keys:

    * ``input`` (required) — ``.raw`` file or directory of snapshots.
    * ``raw`` — substring selecting one ``.raw`` from a directory.
    * ``output_dir`` / ``output_file`` — output location (inferred when
      missing).
    * ``name`` — planning name (default: the ``.raw`` stem).
    * ``gcost`` / ``demand_fail_cost`` / ``scale_objective`` — economic
      knobs (PSS/E carries no costs; see :mod:`psse2gtopt.gtopt_writer`).
    * ``single_bus`` — emit a copper-plate single-bus planning.

    Returns:
        ``0`` on success.
    """
    raw_input = options.get("input")
    if raw_input is None:
        raise ValueError("convert_psse_case: 'input' option is required")
    input_path = Path(raw_input)
    raw_file = resolve_raw_file(input_path, options.get("raw"))

    case = parse_raw(raw_file)

    output_dir = options.get("output_dir")
    output_dir = (
        Path(output_dir) if output_dir else _default_output_dir(raw_file, input_path)
    )
    output_file = options.get("output_file")
    output_file = (
        Path(output_file) if output_file else output_dir / f"{output_dir.name}.json"
    )
    name = options.get("name") or raw_file.stem

    nomenclatura = None
    nom_path = options.get("nomenclatura")
    if nom_path:
        from .aux_data import parse_nomenclatura  # pylint: disable=import-outside-toplevel

        nomenclatura = parse_nomenclatura(Path(nom_path))

    ldm_order = None
    ldm_path = options.get("ldm")
    if ldm_path:
        from .aux_data import parse_ldm_merit_order  # pylint: disable=import-outside-toplevel

        ldm_order = parse_ldm_merit_order(Path(ldm_path))

    planning = build_planning(
        case,
        name=name,
        gcost=options.get("gcost", 10.0),
        gcost_step=options.get("gcost_step", 1.0),
        demand_fail_cost=options.get("demand_fail_cost", 1000.0),
        scale_objective=options.get("scale_objective", 1000.0),
        single_bus=bool(options.get("single_bus", False)),
        rating_set=options.get("rating_set", "A"),
        nomenclatura=nomenclatura,
        ldm_order=ldm_order,
    )

    write_planning(planning, output_file)
    output_dir.mkdir(parents=True, exist_ok=True)
    logger.info("converted %s → %s", raw_file, output_file)
    return 0

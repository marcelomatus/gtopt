"""High-level conversion entry-points for ``plexos2gtopt``.

The functions here are the public façade used by
:mod:`plexos2gtopt.main`:

* :func:`validate_plexos_bundle` — light schema sanity check.
* :func:`convert_plexos_bundle` — full conversion to a gtopt planning
  JSON, end-to-end (bundle → entities → planning).
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .gtopt_writer import build_planning, write_planning
from .parsers import extract_case
from .plexos_loader import locate_bundle


logger = logging.getLogger(__name__)


def validate_plexos_bundle(options: dict[str, Any]) -> bool:
    """Validate a PLEXOS bundle.

    Light sanity check: the bundle path resolves to an extracted
    directory or a recognised archive, and ``DBSEN_PRGDIARIO.xml`` is
    present at the root. Deep schema validation (object counts, class
    coverage) is deferred to the per-extractor logging.

    Args:
        options: Dict with at least ``"input_bundle"`` set.

    Returns:
        ``True`` when the bundle looks well-formed.
    """
    bundle_path = options.get("input_bundle")
    if bundle_path is None:
        logger.error("validate_plexos_bundle: 'input_bundle' option is required")
        return False
    try:
        with locate_bundle(Path(bundle_path)) as bundle:
            if not bundle.xml_path.is_file():
                logger.error("validate failed: %s missing", bundle.xml_path)
                return False
            logger.info("validation OK: %s", bundle.source)
            return True
    except (FileNotFoundError, ValueError) as exc:
        logger.error("validate failed: %s", exc)
        return False


def _resolve_output_paths(
    input_path: Path,
    output_dir: Path | None,
    output_file: Path | None,
    name: str | None,
) -> tuple[Path, Path, str]:
    """Compute ``(output_dir, output_file, planning_name)`` for the run.

    Inference rule (parallel to sddp2gtopt): a ``PLEXOS{date}.zip[.xz]``
    or ``DATOS{date}.zip[.xz]`` input lands in
    ``./gtopt_PLEXOS{date}/`` by default.
    """
    stem = input_path.name
    # Strip recognised compression / archive suffixes for the slug.
    for suffix in (".zip.xz", ".zip"):
        if stem.lower().endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    if output_dir is None:
        output_dir = input_path.parent / f"gtopt_{stem}"
    if output_file is None:
        output_file = output_dir / f"{stem}.json"
    planning_name = name or stem
    return output_dir, output_file, planning_name


def convert_plexos_bundle(options: dict[str, Any]) -> int:
    """Convert a PLEXOS bundle to gtopt JSON.

    Args:
        options: Conversion options. Recognised keys:

            * ``input_bundle`` (required) — path to the PLEXOS bundle.
            * ``output_dir`` — output directory (inferred otherwise).
            * ``output_file`` — explicit JSON path (defaults to
              ``<output_dir>/<stem>.json``).
            * ``name`` — planning name (default: archive stem).
            * ``use_single_bus`` — overrides the multi-bus default.

    Returns:
        Number of CRITICAL findings (``0`` means success).
    """
    raw_input = options.get("input_bundle")
    if raw_input is None:
        raise ValueError("convert_plexos_bundle: 'input_bundle' option is required")
    input_path = Path(raw_input)

    with locate_bundle(input_path) as bundle:
        # Resolve horizon mode + day count + block layout.
        horizon_mode = options.get("horizon_mode") or "plexos"
        horizon_days_opt = options.get("horizon_days")
        block_layout: tuple[tuple[int, ...], ...] = ()

        if horizon_mode == "plexos":
            # Try to find the .accdb sibling and read t_phase_3.
            accdb_path = options.get("plexos_solution_accdb")
            if accdb_path is None:
                from .plexos_block_layout import auto_discover_res_zip

                res_zip = auto_discover_res_zip(input_path)
                if res_zip is not None:
                    from .plexos_block_layout import (
                        load_block_layout_from_res_zip,
                    )

                    block_layout = load_block_layout_from_res_zip(res_zip)
            elif Path(accdb_path).suffix == ".accdb":
                from .plexos_block_layout import load_block_layout_from_accdb

                block_layout = load_block_layout_from_accdb(Path(accdb_path))

            if block_layout:
                # n_days = ceil(max_interval / 24) so the CSV readers
                # extract the full horizon PLEXOS solved over.
                max_iv = max(iv for blk in block_layout for iv in blk)
                bundle.n_days = (max_iv + 23) // 24
                # Also attach the layout onto the loader bundle so
                # ``extract_reservoirs`` can size per-block emin/emax
                # profiles (BundleSpec is constructed AFTER
                # extract_case so the layout isn't otherwise visible
                # there).
                bundle.block_layout = block_layout
                logger.info(
                    "horizon-mode=plexos: %d blocks across %d days "
                    "(loaded from PLEXOS solution)",
                    len(block_layout),
                    bundle.n_days,
                )
            else:
                # No silent fallback.  ``--horizon-mode plexos`` means the
                # caller wants the EXACT block grouping PLEXOS solved
                # over (typically 111 chronological blocks for the CEN
                # PCP daily week).  Falling back to uniform-hourly here
                # would silently produce a bundle that solves a
                # different problem than PLEXOS (e.g. 168 hourly vs 111
                # variable-duration blocks) and any downstream
                # comparison (objective, dispatch, LMP) becomes
                # meaningless.  Make the failure explicit so the user
                # can either supply ``--plexos-solution-accdb`` or
                # opt-in to uniform-hourly via ``--horizon-mode hourly``.
                raise FileNotFoundError(
                    "plexos2gtopt: --horizon-mode=plexos requires the "
                    "PLEXOS solution .accdb to recover the t_phase_3 "
                    "block grouping, but none was found.  Either:\n"
                    "  - pass --plexos-solution-accdb /path/to/Model "
                    "PRGdia_Full_Definitivo Solution.accdb, or\n"
                    "  - place RES<DATE>.zip[.xz] next to the input "
                    "DATOS<DATE>.zip[.xz] so auto-discovery finds it, "
                    "or\n"
                    "  - opt-in to uniform-hourly explicitly with "
                    "--horizon-mode hourly --horizon-days N."
                )
        else:  # hourly
            bundle.n_days = int(horizon_days_opt) if horizon_days_opt else 1

        case = extract_case(bundle)
        # The block layout (if any) rides on the bundle_spec so the
        # writer can pick it up.  ``extract_case`` already populated
        # the bundle_spec; we patch the layout in.
        if block_layout:
            from dataclasses import replace as _dc_replace

            case = _dc_replace(
                case,
                bundle=_dc_replace(case.bundle, block_layout=block_layout),
            )
        output_dir, output_file, planning_name = _resolve_output_paths(
            input_path,
            options.get("output_dir"),
            options.get("output_file"),
            options.get("name"),
        )
        planning = build_planning(
            case,
            name=planning_name,
            default_uc_penalty=options.get("default_uc_penalty"),
        )
        # CLI override goes into model_options (gtopt's nested layout).
        if options.get("use_single_bus"):
            model_opts = planning["options"].setdefault("model_options", {})
            model_opts["use_single_bus"] = True
            model_opts["use_kirchhoff"] = False

        output_dir.mkdir(parents=True, exist_ok=True)
        write_planning(planning, output_file)

        logger.info(
            "converted %s -> %s "
            "(nodes=%d, generators=%d, lines=%d, demands=%d, batteries=%d)",
            input_path,
            output_file,
            len(case.nodes),
            len(case.generators),
            len(case.lines),
            len(case.demands),
            len(case.batteries),
        )
    return 0


__all__ = [
    "convert_plexos_bundle",
    "validate_plexos_bundle",
]

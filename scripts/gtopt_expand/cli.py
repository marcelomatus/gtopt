# -*- coding: utf-8 -*-

"""Command-line interface for the gtopt expansion transforms.

Usage::

    # Stage 2: laja.json → FlowRight/VolumeRight/UserConstraint + laja.pampl
    gtopt_expand laja --input laja.json --output outdir/

    # With explicit stage metadata (calendar months for monthly schedules)
    gtopt_expand maule --input maule.json --output outdir/ \\
        --stages stages.json

    # LNG terminal expansion: lng.json → LngTerminal entities
    gtopt_expand lng --input lng.json --output outdir/ --num-stages 52

    # RoR daily-cycle promotion: ror_equivalence.csv → promoted list
    gtopt_expand ror --planning gtopt.json --output outdir/

The ``--stages`` file must be a JSON document compatible with the
``BaseParser.get_all()`` shape: ``[{"number": 1, "month": 4}, ...]``.

When ``--stages`` is omitted, monthly arrays are emitted in their raw
hydro-year (Laja) or calendar (Maule) form, suitable for cases that don't
need a per-stage materialization.

Exit codes
----------

* ``0`` — success.
* ``2`` — input/validation/IO error (missing file, bad JSON, schema
  violation, template failure).  A one-line ``ERROR:`` is written to
  stderr before exiting.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from gtopt_expand import __version__ as _pkg_version
from gtopt_expand.laja_agreement import LajaAgreement
from gtopt_expand.lng_expand import expand_lng_from_file
from gtopt_expand.maule_agreement import MauleAgreement
from gtopt_expand.pumped_storage_expand import expand_pumped_storage_from_file
from gtopt_expand.ror_expand import DEFAULT_ROR_CSV, expand_ror_from_file


class _StageList:
    """Tiny stage-parser shim for ``LajaAgreement`` / ``MauleAgreement``.

    The agreement classes only call ``get_all()``; this wrapper exposes a
    pre-loaded list of dicts so callers don't need a full ``BaseParser``.
    """

    def __init__(self, stages: list[dict[str, Any]]):
        self._stages = stages

    def get_all(self) -> list[dict[str, Any]]:
        return self._stages


# English month names (as produced by ``plp2gtopt.stage_writer``) and
# Spanish month names (as they appear in PLP source data and Chilean
# hydrology spreadsheets).  ``setiembre`` is the accepted alternate
# spelling of ``septiembre`` still in common use.
_MONTH_NAMES_EN: list[str] = [
    "january",
    "february",
    "march",
    "april",
    "may",
    "june",
    "july",
    "august",
    "september",
    "october",
    "november",
    "december",
]
_MONTH_NAMES_ES: list[str] = [
    "enero",
    "febrero",
    "marzo",
    "abril",
    "mayo",
    "junio",
    "julio",
    "agosto",
    "septiembre",
    "octubre",
    "noviembre",
    "diciembre",
]
_MONTH_NAME_TO_NUMBER: dict[str, int] = {
    name: idx
    for names in (_MONTH_NAMES_EN, _MONTH_NAMES_ES)
    for idx, name in enumerate(names, start=1)
}
# Alternate Spanish spelling for September.
_MONTH_NAME_TO_NUMBER["setiembre"] = 9


def _normalize_month(raw: Any) -> int:
    """Coerce a stage ``month`` field to a 1..12 integer.

    Accepts an int already in range, a decimal string, or an English or
    Spanish month name (e.g. ``"march"`` as produced by
    ``plp2gtopt.stage_writer`` or ``"marzo"`` as it appears in PLP source
    data).  Unknown values fall back to ``1`` so ``_monthly_schedule`` at
    least reads a valid index instead of raising ``KeyError``.
    """
    if isinstance(raw, int):
        return raw if 1 <= raw <= 12 else ((raw - 1) % 12) + 1
    if isinstance(raw, str):
        s = raw.strip().lower()
        if s.isdigit():
            n = int(s)
            return n if 1 <= n <= 12 else ((n - 1) % 12) + 1
        if s in _MONTH_NAME_TO_NUMBER:
            return _MONTH_NAME_TO_NUMBER[s]
    return 1


def _extract_stage_list(data: Any) -> list[dict[str, Any]] | None:
    """Return a stage-dict list from several accepted top-level shapes.

    Accepted forms:

    * bare list of ``{"number": int, "month": int|str}`` dicts — the
      historical ``--stages`` contract;
    * ``{"stages": [...]}`` wrapper around the bare list;
    * a full plp2gtopt planning JSON — returns
      ``data["simulation"]["stage_array"]``.
    """
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        if isinstance(data.get("stages"), list):
            return data["stages"]
        sim = data.get("simulation")
        if isinstance(sim, dict) and isinstance(sim.get("stage_array"), list):
            return sim["stage_array"]
    return None


#: filenames we probe when auto-detecting a companion planning JSON next
#: to the input ``maule.json``.  ``gtopt.json`` is the plp2gtopt default;
#: ``planning.json`` and ``system.json`` are common renames in manual
#: setups.  Order matters — the first hit wins.
_PLANNING_CANDIDATES: tuple[str, ...] = ("gtopt.json", "planning.json", "system.json")


def _extract_reservoir_names(planning: Any) -> set[str]:
    """Return the set of reservoir names declared in a planning JSON.

    Accepts either the raw plp2gtopt planning dict (``{"system": {
    "reservoir_array": [...]}, ...}``) or a bare ``system`` sub-dict.
    Unknown shapes return an empty set so the caller falls back to the
    pasada (default) machicura variant without raising.
    """
    if not isinstance(planning, dict):
        return set()
    system = planning.get("system") if "system" in planning else planning
    if not isinstance(system, dict):
        return set()
    rsv_array = system.get("reservoir_array")
    if not isinstance(rsv_array, list):
        return set()
    names: set[str] = set()
    for entry in rsv_array:
        if isinstance(entry, dict):
            name = entry.get("name")
            if isinstance(name, str) and name:
                names.add(name)
    return names


def _autodetect_planning_path(input_path: Path) -> Path | None:
    """Return a sibling planning JSON next to ``input_path`` if any exists."""
    parent = input_path.parent
    for candidate in _PLANNING_CANDIDATES:
        candidate_path = parent / candidate
        if candidate_path.is_file():
            return candidate_path
    return None


def _load_reservoir_names(
    planning_path: str | None,
    input_path: Path,
) -> set[str]:
    """Resolve the set of reservoir names for the Machicura auto-detection.

    * An explicit ``--planning`` path is always honored (and required to
      exist — a typo is a loud error, not a silent fallback).
    * Otherwise, probe for a sibling file among ``_PLANNING_CANDIDATES``
      next to the input ``maule.json``; missing neighbors silently resolve
      to the pasada default.
    """
    if planning_path is not None:
        path = Path(planning_path)
        if not path.is_file():
            raise FileNotFoundError(f"--planning file not found: {planning_path}")
    else:
        path = _autodetect_planning_path(input_path) or Path("")
        if not path or not path.is_file():
            return set()
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    return _extract_reservoir_names(data)


def _load_stages(path: str | None) -> _StageList | None:
    if path is None:
        return None
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    stages = _extract_stage_list(data)
    if stages is None:
        raise ValueError(
            f"--stages file '{path}' must contain a stage list, a "
            f"{{'stages': [...]}} wrapper, or a planning JSON with "
            f"'simulation.stage_array'"
        )
    # Normalize to the shape _RightsAgreementBase._monthly_schedule expects:
    # each stage must have a ``number`` key and an integer ``month`` in 1..12.
    # ``count_block`` (when present on planning-JSON stage_array entries) is
    # preserved so _to_tb_sched / _to_stb_sched can size the inner block
    # dimension to each stage's own block count — otherwise the emitted
    # schedule would be ``[num_stages][1]`` and gtopt's LP assembly would
    # walk off the inner ``std::vector<double>`` on block indices > 0.
    normalized: list[dict[str, Any]] = []
    for idx, raw in enumerate(stages, start=1):
        stage = dict(raw)
        if "number" not in stage:
            stage["number"] = stage.get("uid", idx)
        stage["month"] = _normalize_month(stage.get("month"))
        normalized.append(stage)
    return _StageList(normalized)


def _emit(
    agreement: Any,
    out_dir: Path,
    artifact_name: str,
) -> Path:
    """Write the agreement's entities and PAMPL file to ``out_dir``.

    The entities are written as ``<artifact_name>_entities.json`` containing
    ``flow_right_array`` / ``volume_right_array``.  The PAMPL file is
    written as ``<artifact_name>.pampl`` (by ``to_json_dict`` itself).
    When the agreement is constraint-only (no inline UserConstraint array
    left over), the ``user_constraint_file`` pointer in the entities
    document references the ``.pampl`` filename — both singular
    ``user_constraint_file`` and plural ``user_constraint_files`` are
    accepted by the ``gtopt`` C++ ``System`` parser.

    Returns the path to the written entities JSON file.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    entities = agreement.to_json_dict(output_dir=out_dir)
    entities_path = out_dir / f"{artifact_name}_entities.json"
    with open(entities_path, "w", encoding="utf-8") as fh:
        json.dump(entities, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return entities_path


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gtopt_expand",
        description=(
            "Expand canonical intermediate JSON files (laja.json,"
            " maule.json, lng.json, pumped_storage configs) into"
            " gtopt entities."
        ),
        epilog=(
            "Subcommands: laja, maule (irrigation agreements →"
            " FlowRight/VolumeRight/UserConstraint + PAMPL); lng"
            " (LNG terminal configuration → LngTerminal entities);"
            " ror (RoR daily-cycle promotion);"
            " pumped_storage (reversible pumped-storage unit →"
            " Turbine + Pump + ReservoirProductionFactor)."
        ),
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {_pkg_version}",
    )
    sub = parser.add_subparsers(dest="subcommand", required=True)

    for name in ("laja", "maule"):
        sp = sub.add_parser(
            name,
            help=f"emit gtopt entities for the {name.capitalize()} agreement",
        )
        sp.add_argument(
            "--input",
            "--in",
            dest="input_path",
            required=True,
            help=f"path to {name}.json",
        )
        sp.add_argument(
            "--output",
            "--out",
            dest="output_dir",
            required=True,
            help="output directory for entities + PAMPL file",
        )
        sp.add_argument(
            "--stages",
            dest="stages_path",
            default=None,
            help=(
                "optional JSON file with per-stage metadata"
                " (list of {number, month} dicts)"
            ),
        )
        sp.add_argument(
            "--last-stage",
            dest="last_stage",
            type=int,
            default=-1,
            help="truncate the stage list to this stage number (default: all)",
        )
        sp.add_argument(
            "--blocks-per-stage",
            dest="blocks_per_stage",
            type=int,
            default=1,
            help="number of blocks per stage (default: 1)",
        )
        if name == "maule":
            sp.add_argument(
                "--planning",
                dest="planning_path",
                default=None,
                help=(
                    "path to the companion plp2gtopt planning JSON used "
                    "to auto-detect the Machicura topology variant "
                    "(embalse when MACHICURA appears in reservoir_array, "
                    "pasada otherwise). When omitted, the CLI probes for "
                    "gtopt.json / planning.json / system.json next to "
                    "--input. Has no effect if cfg['machicura_model'] is "
                    "already set in the input JSON."
                ),
            )
            sp.add_argument(
                "--no-auto-detect-machicura",
                dest="auto_detect_machicura",
                action="store_false",
                default=True,
                help=(
                    "disable the sibling-planning auto-detection for the "
                    "Machicura variant (forces the 'pasada' default "
                    "unless cfg['machicura_model'] is set)."
                ),
            )
    # ── LNG terminal subcommand ──────────────────────────────────────────
    lng_sp = sub.add_parser(
        "lng",
        help="emit gtopt LngTerminal entities from lng.json",
    )
    lng_sp.add_argument(
        "--input",
        "--in",
        dest="input_path",
        required=True,
        help="path to lng.json",
    )
    lng_sp.add_argument(
        "--output",
        "--out",
        dest="output_dir",
        required=True,
        help="output directory for LNG entity JSON",
    )
    lng_sp.add_argument(
        "--num-stages",
        dest="num_stages",
        type=int,
        required=True,
        help="total number of planning stages (for delivery schedule sizing)",
    )
    lng_sp.add_argument(
        "--uid-start",
        dest="uid_start",
        type=int,
        default=1,
        help="first UID to assign to LNG terminals (default: 1)",
    )

    # ── RoR daily-cycle promotion subcommand ─────────────────────────────
    ror_sp = sub.add_parser(
        "ror",
        help="list or expand RoR centrals eligible for daily-cycle promotion",
    )
    ror_sp.add_argument(
        "--input",
        "--in",
        dest="input_path",
        default=str(DEFAULT_ROR_CSV),
        help=(
            "path to ror_equivalence.csv whitelist"
            f" (default: bundled {DEFAULT_ROR_CSV.name})"
        ),
    )
    ror_sp.add_argument(
        "--planning",
        dest="planning_path",
        required=True,
        help="path to the gtopt planning JSON (for central discovery)",
    )
    ror_sp.add_argument(
        "--output",
        "--out",
        dest="output_dir",
        required=True,
        help="output directory for RoR expansion JSON",
    )
    ror_sp.add_argument(
        "--selection",
        dest="selection",
        default="all",
        help=("'all' (default), 'none', or comma-separated central names to promote"),
    )

    # ── Pumped-storage subcommand ───────────────────────────────────────
    ps_sp = sub.add_parser(
        "pumped_storage",
        help=(
            "emit gtopt entities for a reversible pumped-storage unit "
            "from a canonical config file (name/vmin/vmax/PFs/pump_factor)"
        ),
    )
    ps_sp.add_argument(
        "--input",
        "--in",
        dest="input_paths",
        action="append",
        required=True,
        help=(
            "path to a canonical pumped-storage config JSON; may be "
            "repeated to expand multiple units in a single invocation"
        ),
    )
    ps_sp.add_argument(
        "--planning",
        dest="planning_path",
        required=True,
        help=(
            "path to the gtopt planning JSON — used to verify the lower "
            "reservoir exists and, when the config's vmin/vmax are 0 or "
            "absent, to read the upper reservoir's emin/emax"
        ),
    )
    ps_sp.add_argument(
        "--output",
        "--out",
        dest="output_dir",
        required=True,
        help="output directory for per-unit {name}.json files",
    )
    ps_sp.add_argument(
        "--bus",
        dest="bus_name",
        default=None,
        help=(
            "bus for the aux generator + aux demand "
            "(default: lower-cased upper-reservoir name)"
        ),
    )
    ps_sp.add_argument(
        "--uid-start",
        dest="uid_start",
        type=int,
        default=900_000,
        help="first UID to assign for the first unit (default: 900000)",
    )

    return parser


def _run_ror(args: argparse.Namespace) -> Path:
    """Execute the RoR expansion and return the output path."""
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    result = expand_ror_from_file(
        csv_path=args.input_path,
        planning_path=args.planning_path,
        selection=args.selection,
    )
    output_path = out_dir / "ror_promoted.json"
    with open(output_path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return output_path


def _run_pumped_storage(args: argparse.Namespace) -> Path:
    """Execute the pumped-storage expansion(s) and return the output dir.

    Each ``--input FILE`` becomes one unit, emitted as
    ``{output_dir}/{name}.json`` (name from config or filename stem).
    UID ranges are spaced 16 apart per unit so the contiguous 7-element
    block never collides.
    """
    with open(args.planning_path, "r", encoding="utf-8") as fh:
        planning = json.load(fh)
    system = planning.get("system", planning)
    reservoirs_list = system.get("reservoir_array", [])
    reservoirs = reservoirs_list if isinstance(reservoirs_list, list) else []
    reservoir_names = _extract_reservoir_names(planning)

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    last_output: Path | None = None
    for idx, input_path in enumerate(args.input_paths):
        path = Path(input_path)
        fragment = expand_pumped_storage_from_file(
            path,
            reservoirs=reservoirs,
            reservoir_names=reservoir_names,
            bus_name=args.bus_name,
            uid_start=args.uid_start + idx * 16,
        )
        unit_name = fragment["generator_array"][0]["name"].removeprefix("hydro_")
        output_path = out_dir / f"{unit_name}.json"
        with open(output_path, "w", encoding="utf-8") as fh:
            json.dump({"system": fragment}, fh, indent=2, sort_keys=False)
            fh.write("\n")
        last_output = output_path

    assert last_output is not None  # argparse required=True guarantees ≥1 input
    return last_output


def _run_lng(args: argparse.Namespace) -> Path:
    """Execute the LNG expansion and return the entities path."""
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    entities = expand_lng_from_file(
        args.input_path,
        num_stages=args.num_stages,
        uid_start=args.uid_start,
    )
    entities_path = out_dir / "lng_entities.json"
    with open(entities_path, "w", encoding="utf-8") as fh:
        json.dump(entities, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return entities_path


def _run(args: argparse.Namespace) -> Path:
    """Execute the requested conversion and return the entities path."""
    if args.subcommand == "lng":
        return _run_lng(args)
    if args.subcommand == "ror":
        return _run_ror(args)
    if args.subcommand == "pumped_storage":
        return _run_pumped_storage(args)

    stages = _load_stages(args.stages_path)
    options: dict[str, Any] = {
        "last_stage": args.last_stage,
        "blocks_per_stage": args.blocks_per_stage,
    }

    if args.subcommand == "laja":
        agreement: Any = LajaAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "laja"
    else:
        # Machicura topology variant auto-detection: resolve the
        # companion planning JSON (explicit --planning wins, otherwise
        # probe for a sibling file next to --input) and extract its
        # reservoir_array names.  MauleAgreement then picks 'embalse'
        # when junction_retiro is a reservoir in the case, 'pasada'
        # otherwise.  Explicit cfg['machicura_model'] still wins over
        # both sources — see MauleAgreement._machicura_model.
        if args.auto_detect_machicura or args.planning_path is not None:
            reservoir_names = _load_reservoir_names(
                args.planning_path, Path(args.input_path)
            )
            if reservoir_names:
                options["reservoir_names"] = reservoir_names
        agreement = MauleAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "maule"

    out_dir = Path(args.output_dir)
    return _emit(agreement, out_dir, artifact)


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        entities_path = _run(args)
    except FileNotFoundError as exc:
        print(f"ERROR: input file not found: {exc}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as exc:
        print(f"ERROR: invalid JSON in input: {exc}", file=sys.stderr)
        return 2
    except (KeyError, ValueError, TypeError) as exc:
        print(f"ERROR: {args.subcommand}: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"ERROR: I/O failure: {exc}", file=sys.stderr)
        return 2

    if args.subcommand in ("lng", "ror", "pumped_storage"):
        print(f"wrote {entities_path}", file=sys.stderr)
    else:
        pampl_path = entities_path.with_name(f"{args.subcommand}.pampl")
        print(
            f"wrote {entities_path}"
            + (f" and {pampl_path}" if pampl_path.exists() else ""),
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
